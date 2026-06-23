#include "transfer.hpp"

#include "compression.hpp"
#include "mux_receive_session.hpp"
#include "mux_send_scheduler.hpp"
#include "transfer_stream.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <span>

namespace kiko {

using namespace detail;

namespace {

void put_u64(Bytes& out, std::uint64_t value) {
  for (int i = 7; i >= 0; --i) out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xff));
}

void put_u32(Bytes& out, std::uint32_t value) {
  for (int i = 3; i >= 0; --i) out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xff));
}

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
  TransferTiming timing;
  timing.mode = "mux_send";
  timing.mux_channels = n;
  send_transfer_manifest(channels[0], ciphers[0], files);

  for (const auto& entry : files) {
    if (is_symlink_entry(entry)) {
      auto header = make_file_header(entry);
      send_tagged_text_timed(channels[0], ciphers[0], StreamTag::FileHeader, encode_message(header), timing);
      reporter.file_start(entry.relative, 0);

      const auto resume = recv_resume_request(channels[0], ciphers[0], entry);
      send_resume_ack(channels[0], ciphers[0], resume.offset);

      Message end{"end", {{"sha256", kEmptySha256}}};
      send_tagged_text_timed(channels[0], ciphers[0], StreamTag::FileEnd, encode_message(end), timing);
      reporter.file_complete(entry.relative, 0, false);
      continue;
    }

    if (is_dir_entry(entry)) {
      auto header = make_file_header(entry);
      send_tagged_text_timed(channels[0], ciphers[0], StreamTag::FileHeader, encode_message(header), timing);
      reporter.file_start(entry.relative, 0);

      const auto resume = recv_resume_request(channels[0], ciphers[0], entry);
      send_resume_ack(channels[0], ciphers[0], resume.offset);

      Message end{"end", {{"sha256", kEmptySha256}}};
      send_tagged_text_timed(channels[0], ciphers[0], StreamTag::FileEnd, encode_message(end), timing);
      reporter.file_complete(entry.relative, 0, false);
      continue;
    }

    std::ifstream input(entry.absolute, std::ios::binary);
    if (!input) throw KikoError("failed to open input file: " + entry.absolute.string());

    auto header = make_file_header(entry);
    send_tagged_text_timed(channels[0], ciphers[0], StreamTag::FileHeader, encode_message(header), timing);
    reporter.file_start(entry.relative, entry.size);

    const auto resume = recv_resume_request(channels[0], ciphers[0], entry);
    if (resume.complete_skip && resume.offset == entry.size) {
      send_resume_ack(channels[0], ciphers[0], entry.size);
      reporter.status("skipped already-complete " + entry.relative);
      reporter.file_resume(entry.relative, entry.size, entry.size);
      reporter.file_advance(entry.size);
      for (std::size_t k = 0; k < n; ++k) {
        send_tagged_timed(channels[k], ciphers[k], StreamTag::ChunkEnd, std::span<const std::uint8_t>(), timing);
      }
      Message end{"end", {{"sha256", kEmptySha256}}};
      send_tagged_text_timed(channels[0], ciphers[0], StreamTag::FileEnd, encode_message(end), timing);
      grand_total += entry.size;
      reporter.file_complete(entry.relative, entry.size, false);
      continue;
    }

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
    MuxSendScheduler scheduler(channels, ciphers, reporter, timing);
    while (input) {
      const auto read_start = TransferClock::now();
      input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
      add_transfer_elapsed(timing.disk_read_ms, read_start);
      auto got = input.gcount();
      if (got <= 0) break;
      std::span<const std::uint8_t> chunk(buffer.data(), static_cast<std::size_t>(got));
      const auto hash_start = TransferClock::now();
      hasher->update(chunk);
      add_transfer_elapsed(timing.hash_ms, hash_start);
      timing.payload_bytes += static_cast<std::uint64_t>(got);

      Bytes payload;
      payload.reserve(12 + chunk.size());
      put_u64(payload, total);
      put_u32(payload, static_cast<std::uint32_t>(got));
      if (use_zstd) {
        const auto compress_start = TransferClock::now();
        auto compressed = zstd_compress_block(chunk);
        add_transfer_elapsed(timing.compress_ms, compress_start);
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
    send_tagged_text_timed(channels[0], ciphers[0], StreamTag::FileEnd, encode_message(end), timing);
    grand_total += total;
    reporter.file_complete(entry.relative, total, false);
  }

  send_tagged_timed(channels[0], ciphers[0], StreamTag::Done, std::span<const std::uint8_t>(), timing);
  auto ack = recv_tagged(channels[0], ciphers[0]);
  if (!ack || ack->tag != StreamTag::Ack) throw KikoError("expected transfer ack");
  reporter.transfer_timing(timing);
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
  TransferTiming timing;
  timing.mode = "mux_receive";
  timing.mux_channels = n;
  bool saw_manifest = false;
  bool saw_file_header = false;
  std::optional<ReceivePlan> receive_plan;

  while (true) {
    auto frame = recv_tagged(channels[0], ciphers[0]);
    if (!frame) throw KikoError("transfer stream ended unexpectedly");
    if (frame->tag == StreamTag::Manifest) {
      if (saw_file_header) throw KikoError("transfer manifest arrived after file headers");
      if (saw_manifest) throw KikoError("duplicate transfer manifest");
      const auto text = std::string(frame->payload.begin(), frame->payload.end());
      receive_plan = preflight_transfer_manifest(decode_transfer_manifest(text), output_dir, conflict_policy, reporter);
      saw_manifest = true;
      continue;
    }
    if (frame->tag == StreamTag::Done) {
      send_tagged_timed(channels[0], ciphers[0], StreamTag::Ack, std::span<const std::uint8_t>(), timing);
      reporter.transfer_timing(timing);
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
    const auto* planned = receive_plan ? find_receive_plan_entry(&*receive_plan, current_relative) : nullptr;
    if (receive_plan && planned == nullptr) throw KikoError("file header was not listed in manifest: " + current_relative);
    if (planned != nullptr) validate_receive_plan_header(*planned, header, current_relative, declared_size);
    auto current_path = planned != nullptr ? planned->target_path : safe_join(output_dir, current_relative);

    if (is_symlink_header(header)) {
      const auto symlink_target = header.get("target");
      validate_safe_symlink_target(current_relative, symlink_target);
      bool skip_symlink = false;
      if (planned != nullptr && planned->action == ReceivePlanAction::Skip) {
        reporter.status("skipped existing " + current_relative);
        skip_symlink = true;
      } else if (planned != nullptr && planned->action == ReceivePlanAction::Rename) {
        report_renamed_conflict(current_relative, current_path, reporter);
      } else if (planned == nullptr && conflict_policy == ConflictPolicy::Skip && path_exists_no_follow(current_path)) {
        reporter.status("skipped existing " + current_relative);
        skip_symlink = true;
      } else if (planned == nullptr && conflict_policy == ConflictPolicy::Rename && path_exists_no_follow(current_path)) {
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

    if ((planned != nullptr && planned->action == ReceivePlanAction::Skip) ||
        (planned == nullptr && conflict_policy == ConflictPolicy::Skip && path_exists_no_follow(current_path))) {
      reporter.status("skipped existing " + current_relative);
      send_resume(channels[0], ciphers[0], declared_size, {}, true);
      const auto accepted = recv_resume_ack(channels[0], ciphers[0], declared_size, declared_size, current_relative);
      if (accepted != declared_size) throw KikoError("sender rejected conflict skip for " + current_relative);
      reporter.file_start(current_relative, declared_size);
      reporter.file_advance(declared_size);
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

    if (planned != nullptr && planned->action == ReceivePlanAction::Rename) {
      report_renamed_conflict(current_relative, current_path, reporter);
    } else if (planned == nullptr && conflict_policy == ConflictPolicy::Rename && path_exists_no_follow(current_path)) {
      current_path = unique_conflict_path(current_path);
      report_renamed_conflict(current_relative, current_path, reporter);
    }

    if (current_path.has_parent_path()) std::filesystem::create_directories(current_path.parent_path());
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

    MuxReceiveSession receive_session(channels, ciphers, out, part_path, current_relative, declared_size, have,
                                      use_zstd, reporter, timing);
    const auto written = receive_session.receive();

    auto endframe = recv_tagged(channels[0], ciphers[0]);
    if (!endframe || endframe->tag != StreamTag::FileEnd) throw KikoError("expected file end");
    auto trailer = decode_message(std::string(endframe->payload.begin(), endframe->payload.end()));
    auto expected = trailer.get("sha256");
    out.flush();
    out.close();
    if (written > declared_size) throw KikoError("received more data than declared for " + current_relative);
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
