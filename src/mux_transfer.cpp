#include "transfer.hpp"

#include "compression.hpp"
#include "transfer_stream.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <span>
#include <thread>

namespace kiko {

using namespace detail;

namespace {

void put_u64(Bytes& out, std::uint64_t value) {
  for (int i = 7; i >= 0; --i) out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xff));
}

void put_u32(Bytes& out, std::uint32_t value) {
  for (int i = 3; i >= 0; --i) out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xff));
}

std::uint64_t read_u64(const std::uint8_t* p) {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
  return v;
}

std::uint32_t read_u32(const std::uint8_t* p) {
  std::uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v = (v << 8) | p[i];
  return v;
}

struct MuxSendItem {
  Bytes payload;
  std::uint64_t raw_size = 0;
};

class MuxSendScheduler {
 public:
  MuxSendScheduler(std::vector<TcpSocket>& channels, std::vector<StreamCipher>& ciphers, ProgressReporter& reporter)
      : channels_(channels), ciphers_(ciphers), reporter_(reporter), queues_(channels.size()), pending_(channels.size(), 0) {
    workers_.reserve(channels_.size());
    for (std::size_t k = 0; k < channels_.size(); ++k) workers_.emplace_back([this, k] { worker(k); });
  }

  MuxSendScheduler(const MuxSendScheduler&) = delete;
  MuxSendScheduler& operator=(const MuxSendScheduler&) = delete;

  ~MuxSendScheduler() {
    if (!joined_) {
      try {
        finish();
      } catch (...) {
      }
    }
  }

  void enqueue(Bytes payload, std::uint64_t raw_size) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [&] { return error_ || min_pending_locked() <= kMaxPendingPerChannel; });
    if (error_) std::rethrow_exception(error_);

    const auto channel_index = choose_channel_locked();
    pending_[channel_index] += payload.size();
    queues_[channel_index].push_back(MuxSendItem{std::move(payload), raw_size});
    cv_.notify_all();
  }

  void finish() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      finishing_ = true;
    }
    cv_.notify_all();
    for (auto& worker : workers_) {
      if (worker.joinable()) worker.join();
    }
    joined_ = true;
    if (error_) std::rethrow_exception(error_);
  }

 private:
  static constexpr std::uint64_t kMaxPendingPerChannel = 4 * kMuxChunk;

  std::uint64_t min_pending_locked() const {
    std::uint64_t best = pending_.empty() ? 0 : pending_.front();
    for (const auto pending : pending_) {
      if (pending < best) best = pending;
    }
    return best;
  }

  std::size_t choose_channel_locked() {
    std::size_t best = next_channel_;
    for (std::size_t step = 1; step < pending_.size(); ++step) {
      const auto k = (next_channel_ + step) % pending_.size();
      if (pending_[k] < pending_[best]) best = k;
    }
    next_channel_ = (best + 1) % pending_.size();
    return best;
  }

  void set_error(std::exception_ptr error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!error_) error_ = error;
    cv_.notify_all();
  }

  void worker(std::size_t channel_index) {
    while (true) {
      MuxSendItem item;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return error_ || finishing_ || !queues_[channel_index].empty(); });
        if (error_) return;
        if (queues_[channel_index].empty()) {
          if (finishing_) break;
          continue;
        }
        item = std::move(queues_[channel_index].front());
        queues_[channel_index].pop_front();
      }

      try {
        send_tagged(channels_[channel_index], ciphers_[channel_index], StreamTag::Data, item.payload);
        {
          std::lock_guard<std::mutex> lock(report_mutex_);
          reporter_.file_advance(item.raw_size);
        }
      } catch (...) {
        set_error(std::current_exception());
        return;
      }

      {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_[channel_index] -= std::min<std::uint64_t>(pending_[channel_index], item.payload.size());
      }
      cv_.notify_all();
    }

    try {
      send_tagged(channels_[channel_index], ciphers_[channel_index], StreamTag::ChunkEnd,
                  std::span<const std::uint8_t>());
    } catch (...) {
      set_error(std::current_exception());
    }
  }

  std::vector<TcpSocket>& channels_;
  std::vector<StreamCipher>& ciphers_;
  ProgressReporter& reporter_;
  std::vector<std::deque<MuxSendItem>> queues_;
  std::vector<std::uint64_t> pending_;
  std::vector<std::thread> workers_;
  std::mutex mutex_;
  std::mutex report_mutex_;
  std::condition_variable cv_;
  std::exception_ptr error_;
  std::size_t next_channel_ = 0;
  bool finishing_ = false;
  bool joined_ = false;
};

}  // namespace

void send_files_mux(std::vector<TcpSocket>& channels, const SessionKey& key, const std::vector<FileEntry>& files,
                    ProgressReporter& reporter) {
  if (channels.empty()) throw KikoError("mux send requires at least one channel");
  const std::size_t n = channels.size();
  std::vector<StreamCipher> ciphers;
  ciphers.reserve(n);
  for (std::size_t k = 0; k < n; ++k) ciphers.emplace_back(key, true, static_cast<std::uint8_t>(k));

  Bytes buffer(kMuxChunk);
  std::uint64_t grand_total = 0;
  send_transfer_manifest(channels[0], ciphers[0], files);

  for (const auto& entry : files) {
    if (is_symlink_entry(entry)) {
      auto header = make_file_header(entry);
      send_tagged_text(channels[0], ciphers[0], StreamTag::FileHeader, encode_message(header));
      reporter.file_start(entry.relative, 0);

      const auto resume = recv_resume_request(channels[0], ciphers[0], entry);
      send_resume_ack(channels[0], ciphers[0], resume.offset);

      Message end{"end", {{"sha256", kEmptySha256}}};
      send_tagged_text(channels[0], ciphers[0], StreamTag::FileEnd, encode_message(end));
      reporter.file_complete(entry.relative, 0, false);
      continue;
    }

    if (is_dir_entry(entry)) {
      auto header = make_file_header(entry);
      send_tagged_text(channels[0], ciphers[0], StreamTag::FileHeader, encode_message(header));
      reporter.file_start(entry.relative, 0);

      const auto resume = recv_resume_request(channels[0], ciphers[0], entry);
      send_resume_ack(channels[0], ciphers[0], resume.offset);

      Message end{"end", {{"sha256", kEmptySha256}}};
      send_tagged_text(channels[0], ciphers[0], StreamTag::FileEnd, encode_message(end));
      reporter.file_complete(entry.relative, 0, false);
      continue;
    }

    std::ifstream input(entry.absolute, std::ios::binary);
    if (!input) throw KikoError("failed to open input file: " + entry.absolute.string());

    auto header = make_file_header(entry);
    send_tagged_text(channels[0], ciphers[0], StreamTag::FileHeader, encode_message(header));
    reporter.file_start(entry.relative, entry.size);

    const auto resume = recv_resume_request(channels[0], ciphers[0], entry);

    std::optional<Sha256Hasher> hasher;
    hasher.emplace();
    std::string source_prefix_sha256;
    std::uint64_t total = hash_stream_prefix(input, buffer, *hasher, resume.offset, entry.relative,
                                             resume.offset > 0 ? &source_prefix_sha256 : nullptr);
    std::uint64_t offset = resume.offset;
    if (offset > 0 && !resume.prefix_sha256.empty() && resume.prefix_sha256 != source_prefix_sha256) {
      reporter.status("resume prefix mismatch, restarting " + entry.relative);
      input.clear();
      input.seekg(0);
      hasher.emplace();
      total = 0;
      offset = 0;
    }
    send_resume_ack(channels[0], ciphers[0], offset);
    if (offset > 0) {
      reporter.file_resume(entry.relative, offset, entry.size);
      reporter.file_advance(offset);
    }

    const bool use_zstd = should_compress_entry(entry);
    MuxSendScheduler scheduler(channels, ciphers, reporter);
    while (input) {
      input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
      auto got = input.gcount();
      if (got <= 0) break;
      std::span<const std::uint8_t> chunk(buffer.data(), static_cast<std::size_t>(got));
      hasher->update(chunk);

      Bytes payload;
      payload.reserve(12 + chunk.size());
      put_u64(payload, total);
      put_u32(payload, static_cast<std::uint32_t>(got));
      if (use_zstd) {
        auto compressed = zstd_compress_block(chunk);
        payload.insert(payload.end(), compressed.begin(), compressed.end());
      } else {
        payload.insert(payload.end(), chunk.begin(), chunk.end());
      }
      scheduler.enqueue(std::move(payload), static_cast<std::uint64_t>(got));

      total += static_cast<std::uint64_t>(got);
    }

    scheduler.finish();

    auto digest = hasher->finish();
    Message end{"end", {{"sha256", hex_encode(digest)}}};
    send_tagged_text(channels[0], ciphers[0], StreamTag::FileEnd, encode_message(end));
    grand_total += total;
    reporter.file_complete(entry.relative, total, offset > 0);
  }

  send_tagged(channels[0], ciphers[0], StreamTag::Done, std::span<const std::uint8_t>());
  auto ack = recv_tagged(channels[0], ciphers[0]);
  if (!ack || ack->tag != StreamTag::Ack) throw KikoError("expected transfer ack");
  reporter.transfer_complete(files.size(), grand_total);
}

void receive_files_mux(std::vector<TcpSocket>& channels, const SessionKey& key, const std::filesystem::path& output_dir,
                       ProgressReporter& reporter, ConflictPolicy conflict_policy) {
  if (channels.empty()) throw KikoError("mux receive requires at least one channel");
  const std::size_t n = channels.size();
  std::vector<StreamCipher> ciphers;
  ciphers.reserve(n);
  for (std::size_t k = 0; k < n; ++k) ciphers.emplace_back(key, false, static_cast<std::uint8_t>(k));

  std::size_t file_count = 0;
  std::uint64_t grand_total = 0;
  bool saw_manifest = false;
  bool saw_file_header = false;

  while (true) {
    auto frame = recv_tagged(channels[0], ciphers[0]);
    if (!frame) throw KikoError("transfer stream ended unexpectedly");
    if (frame->tag == StreamTag::Manifest) {
      if (saw_file_header) throw KikoError("transfer manifest arrived after file headers");
      if (saw_manifest) throw KikoError("duplicate transfer manifest");
      const auto text = std::string(frame->payload.begin(), frame->payload.end());
      preflight_transfer_manifest(decode_transfer_manifest(text), output_dir, conflict_policy, reporter);
      saw_manifest = true;
      continue;
    }
    if (frame->tag == StreamTag::Done) {
      send_tagged(channels[0], ciphers[0], StreamTag::Ack, std::span<const std::uint8_t>());
      reporter.transfer_complete(file_count, grand_total);
      return;
    }
    if (frame->tag != StreamTag::FileHeader) throw KikoError("expected file header on control channel");
    saw_file_header = true;

    auto header = decode_message(std::string(frame->payload.begin(), frame->payload.end()));
    auto current_relative = header.get("path");
    if (current_relative.empty()) throw KikoError("file header missing path");
    auto declared_size = header.get_u64("size", 0);
    const bool use_zstd = header.get("compress", "zstd") != "none";
    auto current_path = safe_join(output_dir, current_relative);
    if (current_path.has_parent_path()) std::filesystem::create_directories(current_path.parent_path());

    if (is_symlink_header(header)) {
      const auto symlink_target = header.get("target");
      validate_safe_symlink_target(current_relative, symlink_target);
      bool skip_symlink = false;
      if (conflict_policy == ConflictPolicy::Skip && path_exists_no_follow(current_path)) {
        reporter.status("skipped existing " + current_relative);
        skip_symlink = true;
      } else if (conflict_policy == ConflictPolicy::Rename && path_exists_no_follow(current_path)) {
        current_path = unique_conflict_path(current_path);
        report_renamed_conflict(current_relative, current_path, reporter);
      }
      reporter.file_start(current_relative, 0);
      send_resume(channels[0], ciphers[0], 0);
      (void)recv_resume_ack(channels[0], ciphers[0], 0, 0, current_relative);
      auto endframe = recv_tagged(channels[0], ciphers[0]);
      if (!endframe || endframe->tag != StreamTag::FileEnd) throw KikoError("expected file end");
      if (!skip_symlink) create_safe_symlink(current_path, current_relative, symlink_target);
      ++file_count;
      reporter.file_complete(current_relative, 0, false);
      continue;
    }

    if (is_dir_header(current_relative, declared_size)) {
      std::filesystem::create_directories(current_path);
      apply_file_metadata(current_path, header);
      reporter.file_start(current_relative, 0);
      send_resume(channels[0], ciphers[0], 0);
      (void)recv_resume_ack(channels[0], ciphers[0], 0, 0, current_relative);
      auto endframe = recv_tagged(channels[0], ciphers[0]);
      if (!endframe || endframe->tag != StreamTag::FileEnd) throw KikoError("expected file end");
      ++file_count;
      reporter.file_complete(current_relative, 0, false);
      continue;
    }

    if (conflict_policy == ConflictPolicy::Skip && path_exists_no_follow(current_path)) {
      reporter.status("skipped existing " + current_relative);
      send_resume(channels[0], ciphers[0], declared_size);
      const auto accepted = recv_resume_ack(channels[0], ciphers[0], declared_size, declared_size, current_relative);
      if (accepted != declared_size) throw KikoError("sender rejected conflict skip for " + current_relative);
      reporter.file_start(current_relative, declared_size);
      for (std::size_t k = 0; k < n; ++k) {
        auto chunk_end = recv_tagged(channels[k], ciphers[k]);
        if (!chunk_end || chunk_end->tag != StreamTag::ChunkEnd) throw KikoError("expected mux chunk end");
      }
      auto endframe = recv_tagged(channels[0], ciphers[0]);
      if (!endframe || endframe->tag != StreamTag::FileEnd) throw KikoError("expected file end");
      ++file_count;
      grand_total += declared_size;
      reporter.file_complete(current_relative, declared_size, true);
      continue;
    }

    if (try_skip_existing_duplicate(channels[0], ciphers[0], header, current_path, current_relative, declared_size,
                                    reporter)) {
      for (std::size_t k = 0; k < n; ++k) {
        auto chunk_end = recv_tagged(channels[k], ciphers[k]);
        if (!chunk_end || chunk_end->tag != StreamTag::ChunkEnd) throw KikoError("expected mux chunk end");
      }
      auto endframe = recv_tagged(channels[0], ciphers[0]);
      if (!endframe || endframe->tag != StreamTag::FileEnd) throw KikoError("expected file end");
      ++file_count;
      grand_total += declared_size;
      reporter.file_complete(current_relative, declared_size, true);
      continue;
    }

    if (conflict_policy == ConflictPolicy::Rename && path_exists_no_follow(current_path)) {
      current_path = unique_conflict_path(current_path);
      report_renamed_conflict(current_relative, current_path, reporter);
      if (current_path.has_parent_path()) std::filesystem::create_directories(current_path.parent_path());
    }

    auto part_path = part_path_for(current_path);

    auto have = resumable_part_size(part_path, declared_size);
    std::string prefix_sha256;
    if (have > 0) {
      Bytes prefix_buffer(kMuxChunk);
      Sha256Hasher prefix_hasher;
      if (!hash_existing_part_prefix(part_path, have, prefix_buffer, prefix_hasher, &prefix_sha256)) {
        have = 0;
        prefix_sha256.clear();
      }
    }

    send_resume(channels[0], ciphers[0], have, prefix_sha256);
    const auto accepted = recv_resume_ack(channels[0], ciphers[0], have, declared_size, current_relative);
    if (accepted != have) {
      reporter.status("resume rejected, restarting " + current_relative);
      have = accepted;
    }

    if (have == 0) {
      std::ofstream create(part_path, std::ios::binary | std::ios::trunc);
    }
    std::fstream out(part_path, std::ios::binary | std::ios::in | std::ios::out);
    if (!out) throw KikoError("failed to open output file: " + part_path.string());

    reporter.file_start(current_relative, declared_size);
    if (have > 0) {
      reporter.file_resume(current_relative, have, declared_size);
      reporter.file_advance(have);
    }

    std::mutex file_mutex;
    std::mutex report_mutex;
    std::atomic<std::uint64_t> written{have};
    std::atomic<bool> failed{false};
    std::string error_text;

    auto reader = [&, use_zstd](std::size_t k) {
      try {
        while (true) {
          auto f = recv_tagged(channels[k], ciphers[k]);
          if (!f) throw KikoError("data channel closed early");
          if (f->tag == StreamTag::ChunkEnd) break;
          if (f->tag != StreamTag::Data) throw KikoError("unexpected frame on data channel");
          if (f->payload.size() < 12) throw KikoError("short data frame");
          std::uint64_t off = read_u64(f->payload.data());
          std::uint32_t raw_len = read_u32(f->payload.data() + 8);
          if (raw_len > kMuxChunk) throw KikoError("mux data chunk too large for " + current_relative);
          if (off < have) throw KikoError("mux data before resume offset for " + current_relative);
          ensure_declared_space(off, declared_size, static_cast<std::uint64_t>(raw_len), current_relative);
          std::span<const std::uint8_t> comp(f->payload.data() + 12, f->payload.size() - 12);
          Bytes plain;
          if (use_zstd) {
            plain = zstd_decompress_block(comp, raw_len);
          } else {
            if (comp.size() != raw_len) throw KikoError("uncompressed chunk size mismatch");
            plain.assign(comp.begin(), comp.end());
          }
          if (plain.size() != raw_len) throw KikoError("mux decoded chunk size mismatch");
          ensure_declared_space(off, declared_size, static_cast<std::uint64_t>(plain.size()), current_relative);
          {
            std::lock_guard<std::mutex> lock(file_mutex);
            out.seekp(static_cast<std::streamoff>(off));
            out.write(reinterpret_cast<const char*>(plain.data()), static_cast<std::streamsize>(plain.size()));
            if (!out) throw KikoError("write failed during mux receive");
          }
          written.fetch_add(plain.size());
          std::lock_guard<std::mutex> rlock(report_mutex);
          reporter.file_advance(plain.size());
        }
      } catch (const std::exception& e) {
        failed.store(true);
        std::lock_guard<std::mutex> rlock(report_mutex);
        if (error_text.empty()) error_text = e.what();
      }
    };

    std::vector<std::thread> threads;
    threads.reserve(n - 1);
    for (std::size_t k = 1; k < n; ++k) threads.emplace_back(reader, k);
    reader(0);
    for (auto& t : threads) t.join();
    if (failed.load()) throw KikoError("mux receive failed: " + error_text);

    auto endframe = recv_tagged(channels[0], ciphers[0]);
    if (!endframe || endframe->tag != StreamTag::FileEnd) throw KikoError("expected file end");
    auto trailer = decode_message(std::string(endframe->payload.begin(), endframe->payload.end()));
    auto expected = trailer.get("sha256");
    out.flush();
    out.close();
    if (written.load() > declared_size) throw KikoError("received more data than declared for " + current_relative);
    Bytes verify_buffer(kMuxChunk);
    verify_part_file_digest(part_path, current_relative, declared_size, expected, verify_buffer);
    finalize_part_file(part_path, current_path, current_relative);
    apply_file_metadata(current_path, header);
    ++file_count;
    grand_total += declared_size;
    reporter.file_complete(current_relative, declared_size, true);
  }
}

}  // namespace kiko
