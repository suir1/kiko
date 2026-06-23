#include "transfer_stream.hpp"

#include "core/compression.hpp"
#include "transfer_heuristics.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <limits>

namespace kiko::detail {

void ensure_declared_space(std::uint64_t current_total, std::uint64_t declared_size, std::uint64_t next_size,
                           const std::string& relative) {
  if (current_total > declared_size || next_size > declared_size - current_total) {
    throw KikoError("received more data than declared for " + relative);
  }
}

std::size_t declared_remaining_limit(std::uint64_t current_total, std::uint64_t declared_size,
                                     const std::string& relative) {
  ensure_declared_space(current_total, declared_size, 0, relative);
  const auto remaining = declared_size - current_total;
  const auto max_size = static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
  return remaining > max_size ? std::numeric_limits<std::size_t>::max() : static_cast<std::size_t>(remaining);
}

bool is_dir_entry(const FileEntry& entry) {
  return entry.size == 0 && !entry.relative.empty() && entry.relative.back() == '/';
}

bool is_symlink_entry(const FileEntry& entry) {
  return entry.symlink;
}

void append_mtime_field(Message& header, const FileEntry& entry) {
  if (entry.mtime_ms > 0) header.fields["mtime_ms"] = std::to_string(entry.mtime_ms);
}

void append_mode_field(Message& header, const FileEntry& entry) {
  if (entry.mode > 0) header.fields["mode"] = std::to_string(entry.mode);
}

bool should_compress_entry(const FileEntry& entry) {
  return !is_dir_entry(entry) && !is_symlink_entry(entry) && should_compress_path(entry.absolute);
}

Message make_file_header(const FileEntry& entry) {
  if (is_symlink_entry(entry)) {
    return Message{"file",
                   {{"path", entry.relative},
                    {"size", "0"},
                    {"compress", "none"},
                    {"kind", "symlink"},
                    {"target", entry.link_target}}};
  }
  if (is_dir_entry(entry)) {
    Message header{"file", {{"path", entry.relative}, {"size", "0"}, {"compress", "none"}}};
    append_mtime_field(header, entry);
    append_mode_field(header, entry);
    return header;
  }

  Message header{"file",
                 {{"path", entry.relative},
                  {"size", std::to_string(entry.size)},
                  {"imohash", entry.imohash},
                  {"compress", should_compress_entry(entry) ? "zstd" : "none"}}};
  append_mtime_field(header, entry);
  append_mode_field(header, entry);
  return header;
}

void send_transfer_manifest(TcpSocket& socket, StreamCipher& cipher, const std::vector<FileEntry>& files) {
  send_tagged_text(socket, cipher, StreamTag::Manifest, encode_transfer_manifest(files));
}

void send_tagged(TcpSocket& socket, StreamCipher& cipher, StreamTag tag, std::span<const std::uint8_t> payload) {
  Bytes plain;
  plain.reserve(payload.size() + 1);
  plain.push_back(static_cast<std::uint8_t>(tag));
  plain.insert(plain.end(), payload.begin(), payload.end());
  send_frame(socket, cipher.encrypt(plain));
}

void send_tagged_text(TcpSocket& socket, StreamCipher& cipher, StreamTag tag, const std::string& text) {
  send_tagged(socket, cipher, tag,
              std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

std::optional<TaggedFrame> recv_tagged(TcpSocket& socket, StreamCipher& cipher) {
  auto encrypted = recv_frame(socket);
  if (!encrypted) return std::nullopt;
  auto plain = cipher.decrypt(*encrypted);
  if (plain.empty()) throw KikoError("malformed transfer frame");
  TaggedFrame frame;
  frame.tag = static_cast<StreamTag>(plain[0]);
  frame.payload.assign(plain.begin() + 1, plain.end());
  return frame;
}

std::int64_t transfer_elapsed_ms_since(TransferClock::time_point start) {
  const auto elapsed = TransferClock::now() - start;
  return std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
}

void add_transfer_elapsed(std::int64_t& bucket, TransferClock::time_point start) {
  bucket += transfer_elapsed_ms_since(start);
}

void send_tagged_timed(TcpSocket& socket, StreamCipher& cipher, StreamTag tag,
                       std::span<const std::uint8_t> payload, TransferTiming& timing) {
  const auto start = TransferClock::now();
  send_tagged(socket, cipher, tag, payload);
  const auto elapsed = transfer_elapsed_ms_since(start);
  timing.send_frame_ms += elapsed;
  timing.max_send_frame_ms = std::max(timing.max_send_frame_ms, static_cast<int>(elapsed));
  ++timing.frame_count;
}

void send_tagged_text_timed(TcpSocket& socket, StreamCipher& cipher, StreamTag tag, const std::string& text,
                            TransferTiming& timing) {
  send_tagged_timed(socket, cipher, tag,
                    std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()),
                    timing);
}

}  // namespace kiko::detail

namespace kiko {

using namespace detail;

void send_files(TcpSocket& channel, const SessionKey& key, const std::vector<FileEntry>& files,
                ProgressReporter& reporter) {
  StreamCipher cipher(key, /*sender_originates=*/true);
  Bytes buffer(kPlainChunk);
  std::uint64_t grand_total = 0;
  TransferTiming timing;
  timing.mode = "stream_send";
  send_transfer_manifest(channel, cipher, files);

  for (const auto& entry : files) {
    if (is_symlink_entry(entry)) {
      auto header = make_file_header(entry);
      send_tagged_text_timed(channel, cipher, StreamTag::FileHeader, encode_message(header), timing);
      reporter.file_start(entry.relative, 0);

      const auto resume = recv_resume_request(channel, cipher, entry);
      send_resume_ack(channel, cipher, resume.offset);

      Message end{"end", {{"sha256", kEmptySha256}}};
      send_tagged_text_timed(channel, cipher, StreamTag::FileEnd, encode_message(end), timing);
      reporter.file_complete(entry.relative, 0, false);
      continue;
    }

    if (is_dir_entry(entry)) {
      auto header = make_file_header(entry);
      send_tagged_text_timed(channel, cipher, StreamTag::FileHeader, encode_message(header), timing);
      reporter.file_start(entry.relative, 0);

      const auto resume = recv_resume_request(channel, cipher, entry);
      send_resume_ack(channel, cipher, resume.offset);

      Message end{"end", {{"sha256", kEmptySha256}}};
      send_tagged_text_timed(channel, cipher, StreamTag::FileEnd, encode_message(end), timing);
      reporter.file_complete(entry.relative, 0, false);
      continue;
    }

    std::ifstream input(entry.absolute, std::ios::binary);
    if (!input) throw KikoError("failed to open input file: " + entry.absolute.string());

    auto header = make_file_header(entry);
    send_tagged_text_timed(channel, cipher, StreamTag::FileHeader, encode_message(header), timing);
    reporter.file_start(entry.relative, entry.size);

    // The receiver replies with how many leading bytes it already has so we can
    // resume an interrupted transfer instead of resending the whole file.
    const auto resume = recv_resume_request(channel, cipher, entry);
    if (resume.complete_skip && resume.offset == entry.size) {
      send_resume_ack(channel, cipher, entry.size);
      reporter.status("skipped already-complete " + entry.relative);
      reporter.file_resume(entry.relative, entry.size, entry.size);
      reporter.file_advance(entry.size);
      Message end{"end", {{"sha256", kEmptySha256}}};
      send_tagged_text_timed(channel, cipher, StreamTag::FileEnd, encode_message(end), timing);
      grand_total += entry.size;
      reporter.file_complete(entry.relative, entry.size, false);
      continue;
    }

    std::optional<Sha256Hasher> hasher;
    hasher.emplace();
    // Hash (without resending) the prefix the receiver already holds so the
    // final digest still covers the whole file. If the receiver provided a
    // prefix digest and it does not match the local source, restart this file
    // from byte 0 instead of wasting the rest of the transfer.
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
    send_resume_ack(channel, cipher, offset);
    if (offset > 0) {
      reporter.file_resume(entry.relative, offset, entry.size);
      reporter.file_advance(offset);
    }

    const bool use_zstd = should_compress_entry(entry);
    std::optional<ZstdStreamCompressor> compressor;
    if (use_zstd) compressor.emplace(3);
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
      total += static_cast<std::uint64_t>(got);
      timing.payload_bytes += static_cast<std::uint64_t>(got);
      if (use_zstd) {
        const auto compress_start = TransferClock::now();
        auto compressed = compressor->compress(chunk, false);
        add_transfer_elapsed(timing.compress_ms, compress_start);
        if (!compressed.empty()) send_tagged_timed(channel, cipher, StreamTag::Data, compressed, timing);
      } else {
        send_tagged_timed(channel, cipher, StreamTag::Data, chunk, timing);
      }
      reporter.file_advance(static_cast<std::uint64_t>(got));
    }
    if (use_zstd) {
      const auto compress_start = TransferClock::now();
      auto trailer = compressor->compress(std::span<const std::uint8_t>(), true);
      add_transfer_elapsed(timing.compress_ms, compress_start);
      if (!trailer.empty()) send_tagged_timed(channel, cipher, StreamTag::Data, trailer, timing);
    }

    auto digest = hasher->finish();
    Message end{"end", {{"sha256", hex_encode(digest)}}};
    send_tagged_text_timed(channel, cipher, StreamTag::FileEnd, encode_message(end), timing);
    grand_total += total;
    reporter.file_complete(entry.relative, total, false);
  }

  send_tagged_timed(channel, cipher, StreamTag::Done, std::span<const std::uint8_t>(), timing);
  auto ack = recv_tagged(channel, cipher);
  if (!ack || ack->tag != StreamTag::Ack) throw KikoError("expected transfer ack");
  reporter.transfer_timing(timing);
  reporter.transfer_complete(files.size(), grand_total);
}

void receive_files(TcpSocket& channel, const SessionKey& key, const std::filesystem::path& output_dir,
                   ProgressReporter& reporter, ConflictPolicy conflict_policy) {
  StreamCipher cipher(key, /*sender_originates=*/false);
  Bytes buffer(kPlainChunk);
  TransferTiming timing;
  timing.mode = "stream_receive";

  std::ofstream out;
  std::optional<ZstdStreamDecompressor> decompressor;
  std::optional<Sha256Hasher> hasher;
  bool use_zstd = true;
  std::filesystem::path current_path;
  std::filesystem::path part_path;
  std::string current_relative;
  std::uint64_t current_total = 0;
  std::uint64_t current_declared_size = 0;
  bool skipping_file = false;
  bool is_dir_marker = false;
  bool is_symlink_marker = false;
  bool skip_symlink_marker = false;
  std::string pending_symlink_target;
  std::uint64_t pending_mtime_ms = 0;
  std::uint32_t pending_mode = 0;
  std::size_t file_count = 0;
  std::uint64_t grand_total = 0;
  bool saw_manifest = false;
  bool saw_file_header = false;
  std::optional<ReceivePlan> receive_plan;

  while (true) {
    auto frame = recv_tagged(channel, cipher);
    if (!frame) throw KikoError("transfer stream ended unexpectedly");

    switch (frame->tag) {
      case StreamTag::Manifest: {
        if (saw_file_header) throw KikoError("transfer manifest arrived after file headers");
        if (saw_manifest) throw KikoError("duplicate transfer manifest");
        const auto text = std::string(frame->payload.begin(), frame->payload.end());
        receive_plan = preflight_transfer_manifest(decode_transfer_manifest(text), output_dir, conflict_policy, reporter);
        saw_manifest = true;
        break;
      }
      case StreamTag::FileHeader: {
        saw_file_header = true;
        auto header = decode_message(std::string(frame->payload.begin(), frame->payload.end()));
        current_relative = header.get("path");
        if (current_relative.empty()) throw KikoError("file header missing path");
        auto declared_size = header.get_u64("size", 0);
        current_declared_size = declared_size;
        use_zstd = header.get("compress", "zstd") != "none";
        const auto* planned = receive_plan ? find_receive_plan_entry(&*receive_plan, current_relative) : nullptr;
        if (receive_plan && planned == nullptr) throw KikoError("file header was not listed in manifest: " + current_relative);
        if (planned != nullptr) validate_receive_plan_header(*planned, header, current_relative, declared_size);
        current_path = planned != nullptr ? planned->target_path : safe_join(output_dir, current_relative);

        if (is_symlink_header(header)) {
          pending_symlink_target = header.get("target");
          validate_safe_symlink_target(current_relative, pending_symlink_target);
          skip_symlink_marker = false;
          if (planned != nullptr && planned->action == ReceivePlanAction::Skip) {
            reporter.status("skipped existing " + current_relative);
            skip_symlink_marker = true;
          } else if (planned != nullptr && planned->action == ReceivePlanAction::Rename) {
            report_renamed_conflict(current_relative, current_path, reporter);
          } else if (planned == nullptr && conflict_policy == ConflictPolicy::Skip && path_exists_no_follow(current_path)) {
            reporter.status("skipped existing " + current_relative);
            skip_symlink_marker = true;
          } else if (planned == nullptr && conflict_policy == ConflictPolicy::Rename && path_exists_no_follow(current_path)) {
            current_path = unique_conflict_path(current_path);
            report_renamed_conflict(current_relative, current_path, reporter);
          }
          send_resume(channel, cipher, 0);
          (void)recv_resume_ack(channel, cipher, 0, 0, current_relative);
          reporter.file_start(current_relative, 0);
          is_symlink_marker = true;
          current_total = 0;
          current_declared_size = 0;
          pending_mtime_ms = 0;
          pending_mode = 0;
          break;
        }

        if (is_dir_header(current_relative, declared_size)) {
          std::filesystem::create_directories(current_path);
          apply_file_metadata(current_path, header);
          send_resume(channel, cipher, 0);
          (void)recv_resume_ack(channel, cipher, 0, 0, current_relative);
          reporter.file_start(current_relative, 0);
          is_dir_marker = true;
          current_total = 0;
          current_declared_size = 0;
          pending_mtime_ms = 0;
          pending_mode = 0;
          break;
        }

        pending_mtime_ms = header.get_u64("mtime_ms", 0);
        pending_mode = static_cast<std::uint32_t>(header.get_u64("mode", 0));

        if ((planned != nullptr && planned->action == ReceivePlanAction::Skip) ||
            (planned == nullptr && conflict_policy == ConflictPolicy::Skip && path_exists_no_follow(current_path))) {
          reporter.status("skipped existing " + current_relative);
          send_resume(channel, cipher, declared_size, {}, true);
          const auto accepted = recv_resume_ack(channel, cipher, declared_size, declared_size, current_relative);
          if (accepted != declared_size) throw KikoError("sender rejected conflict skip for " + current_relative);
          reporter.file_start(current_relative, declared_size);
          reporter.file_advance(declared_size);
          skipping_file = true;
          current_total = declared_size;
          break;
        }

        if (try_skip_existing_duplicate(channel, cipher, header, current_path, current_relative, declared_size, reporter)) {
          skipping_file = true;
          current_total = declared_size;
          break;
        }

        if (planned != nullptr && planned->action == ReceivePlanAction::Rename) {
          report_renamed_conflict(current_relative, current_path, reporter);
        } else if (planned == nullptr && conflict_policy == ConflictPolicy::Rename && path_exists_no_follow(current_path)) {
          current_path = unique_conflict_path(current_path);
          report_renamed_conflict(current_relative, current_path, reporter);
        }

        if (current_path.has_parent_path()) std::filesystem::create_directories(current_path.parent_path());
        part_path = part_path_for(current_path);

        // Resume from an existing partial file when present and not larger than
        // the file we are about to receive.
        std::uint64_t have = resumable_part_size(part_path, declared_size);
        hasher.emplace();
        std::string prefix_sha256;
        if (have > 0) {
          if (!hash_existing_part_prefix(part_path, have, buffer, *hasher, &prefix_sha256)) {
            have = 0;
            prefix_sha256.clear();
            hasher.emplace();
          }
        }

        send_resume(channel, cipher, have, prefix_sha256);
        const auto accepted = recv_resume_ack(channel, cipher, have, declared_size, current_relative);
        if (accepted != have) {
          reporter.status("resume rejected, restarting " + current_relative);
          have = accepted;
          hasher.emplace();
          if (have > 0 && !hash_existing_part_prefix(part_path, have, buffer, *hasher)) {
            have = 0;
            hasher.emplace();
          }
        }

        out = std::ofstream(part_path, std::ios::binary | (have > 0 ? std::ios::app : std::ios::trunc));
        if (!out) throw KikoError("failed to open output file: " + part_path.string());
        if (use_zstd) decompressor.emplace();
        current_total = have;
        reporter.file_start(current_relative, declared_size);
        if (have > 0) {
          reporter.file_resume(current_relative, have, declared_size);
          reporter.file_advance(have);
        }
        break;
      }
      case StreamTag::Data: {
        if (skipping_file) break;
        if (!hasher) throw KikoError("data frame before file header");
        if (use_zstd) {
          if (!decompressor) throw KikoError("data frame before decompressor ready");
          const auto decompress_start = TransferClock::now();
          auto decompressed = decompressor->decompress(
              frame->payload, declared_remaining_limit(current_total, current_declared_size, current_relative));
          add_transfer_elapsed(timing.decompress_ms, decompress_start);
          if (!decompressed.empty()) {
            const auto write_start = TransferClock::now();
            out.write(reinterpret_cast<const char*>(decompressed.data()), static_cast<std::streamsize>(decompressed.size()));
            add_transfer_elapsed(timing.disk_write_ms, write_start);
            const auto hash_start = TransferClock::now();
            hasher->update(decompressed);
            add_transfer_elapsed(timing.hash_ms, hash_start);
            current_total += decompressed.size();
            timing.payload_bytes += decompressed.size();
            reporter.file_advance(decompressed.size());
          }
        } else {
          if (!frame->payload.empty()) {
            ensure_declared_space(current_total, current_declared_size,
                                  static_cast<std::uint64_t>(frame->payload.size()), current_relative);
            const auto write_start = TransferClock::now();
            out.write(reinterpret_cast<const char*>(frame->payload.data()),
                      static_cast<std::streamsize>(frame->payload.size()));
            add_transfer_elapsed(timing.disk_write_ms, write_start);
            const auto hash_start = TransferClock::now();
            hasher->update(frame->payload);
            add_transfer_elapsed(timing.hash_ms, hash_start);
            current_total += frame->payload.size();
            timing.payload_bytes += frame->payload.size();
            reporter.file_advance(frame->payload.size());
          }
        }
        break;
      }
      case StreamTag::FileEnd: {
        if (is_symlink_marker) {
          if (!skip_symlink_marker) create_safe_symlink(current_path, current_relative, pending_symlink_target);
          ++file_count;
          reporter.file_complete(current_relative, 0, false);
          is_symlink_marker = false;
          skip_symlink_marker = false;
          pending_symlink_target.clear();
          break;
        }
        if (is_dir_marker) {
          ++file_count;
          reporter.file_complete(current_relative, 0, false);
          is_dir_marker = false;
          break;
        }
        if (skipping_file) {
          ++file_count;
          grand_total += current_total;
          reporter.file_complete(current_relative, current_total, true);
          skipping_file = false;
          current_total = 0;
          current_declared_size = 0;
          pending_mtime_ms = 0;
          pending_mode = 0;
          break;
        }
        if (!hasher) throw KikoError("file end before file header");
        out.flush();
        out.close();
        auto trailer = decode_message(std::string(frame->payload.begin(), frame->payload.end()));
        auto expected = trailer.get("sha256");
        auto actual = hex_encode(hasher->finish());
        verify_received_digest(part_path, current_relative, current_total, current_declared_size, expected, actual);
        finalize_part_file(part_path, current_path, current_relative);
        apply_file_mode_bits(current_path, pending_mode);
        apply_mtime_ms(current_path, pending_mtime_ms);
        pending_mtime_ms = 0;
        pending_mode = 0;
        ++file_count;
        grand_total += current_total;
        reporter.file_complete(current_relative, current_total, true);
        current_total = 0;
        current_declared_size = 0;
        decompressor.reset();
        hasher.reset();
        break;
      }
      case StreamTag::Done:
        send_tagged_timed(channel, cipher, StreamTag::Ack, std::span<const std::uint8_t>(), timing);
        reporter.transfer_timing(timing);
        reporter.transfer_complete(file_count, grand_total);
        return;
      default:
        throw KikoError("unknown transfer frame tag");
    }
  }
}

}  // namespace kiko
