#include "transfer_stream.hpp"

#include "compression.hpp"
#include "imohash.hpp"
#include "transfer_heuristics.hpp"

#include <algorithm>
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

bool is_dir_header(const std::string& path, std::uint64_t size) {
  return size == 0 && !path.empty() && path.back() == '/';
}

void append_mtime_field(Message& header, const FileEntry& entry) {
  if (entry.mtime_ms > 0) header.fields["mtime_ms"] = std::to_string(entry.mtime_ms);
}

bool should_compress_entry(const FileEntry& entry) {
  return !is_dir_entry(entry) && should_compress_path(entry.absolute);
}

Message make_file_header(const FileEntry& entry) {
  if (is_dir_entry(entry)) {
    Message header{"file", {{"path", entry.relative}, {"size", "0"}, {"compress", "none"}}};
    append_mtime_field(header, entry);
    return header;
  }

  Message header{"file",
                 {{"path", entry.relative},
                  {"size", std::to_string(entry.size)},
                  {"imohash", entry.imohash},
                  {"compress", should_compress_entry(entry) ? "zstd" : "none"}}};
  append_mtime_field(header, entry);
  return header;
}

void send_resume(TcpSocket& socket, StreamCipher& cipher, std::uint64_t offset, const std::string& prefix_sha256) {
  Message resume{"resume", {{"offset", std::to_string(offset)}}};
  if (!prefix_sha256.empty()) resume.fields["prefix_sha256"] = prefix_sha256;
  send_tagged_text(socket, cipher, StreamTag::Resume, encode_message(resume));
}

ResumeRequest recv_resume_request(TcpSocket& socket, StreamCipher& cipher, const FileEntry& entry) {
  auto resume = recv_tagged(socket, cipher);
  if (!resume || resume->tag != StreamTag::Resume) throw KikoError("expected resume frame");
  auto resume_msg = decode_message(std::string(resume->payload.begin(), resume->payload.end()));
  auto offset = resume_msg.get_u64("offset", 0);
  return ResumeRequest{offset > entry.size ? 0 : offset, resume_msg.get("prefix_sha256")};
}

void send_resume_ack(TcpSocket& socket, StreamCipher& cipher, std::uint64_t accepted_offset) {
  Message ack{"resume_ack", {{"offset", std::to_string(accepted_offset)}}};
  send_tagged_text(socket, cipher, StreamTag::ResumeAck, encode_message(ack));
}

std::uint64_t recv_resume_ack(TcpSocket& socket, StreamCipher& cipher, std::uint64_t requested_offset,
                              std::uint64_t declared_size, const std::string& relative) {
  auto ack = recv_tagged(socket, cipher);
  if (!ack || ack->tag != StreamTag::ResumeAck) throw KikoError("expected resume ack");
  auto ack_msg = decode_message(std::string(ack->payload.begin(), ack->payload.end()));
  auto accepted = ack_msg.get_u64("offset", 0);
  if (accepted > declared_size || accepted > requested_offset) {
    throw KikoError("invalid resume ack for " + relative);
  }
  return accepted;
}

std::uint64_t hash_stream_prefix(std::istream& input, Bytes& buffer, Sha256Hasher& hasher, std::uint64_t offset,
                                 const std::string& relative, std::string* prefix_sha256) {
  std::optional<Sha256Hasher> prefix_hasher;
  if (prefix_sha256 != nullptr) prefix_hasher.emplace();
  std::uint64_t total = 0;
  while (total < offset) {
    auto want = std::min<std::uint64_t>(buffer.size(), offset - total);
    input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(want));
    auto got = input.gcount();
    if (got <= 0) throw KikoError("source shorter than resume offset: " + relative);
    std::span<const std::uint8_t> chunk(buffer.data(), static_cast<std::size_t>(got));
    hasher.update(chunk);
    if (prefix_hasher) prefix_hasher->update(chunk);
    total += static_cast<std::uint64_t>(got);
  }
  if (prefix_hasher) *prefix_sha256 = hex_encode(prefix_hasher->finish());
  return total;
}

bool try_skip_existing_duplicate(TcpSocket& socket, StreamCipher& cipher, const Message& header,
                                 const std::filesystem::path& current_path, const std::string& current_relative,
                                 std::uint64_t declared_size, ProgressReporter& reporter) {
  const auto declared_imohash = header.get("imohash");
  if (declared_imohash.empty()) return false;

  std::error_code exists_ec;
  if (!std::filesystem::exists(current_path, exists_ec) || exists_ec) return false;

  try {
    if (imohash_hex(current_path) != declared_imohash) return false;
  } catch (...) {
    return false;
  }

  send_resume(socket, cipher, declared_size);
  const auto accepted = recv_resume_ack(socket, cipher, declared_size, declared_size, current_relative);
  if (accepted != declared_size) return false;
  reporter.status("skipped duplicate " + current_relative);
  reporter.file_start(current_relative, declared_size);
  return true;
}

std::filesystem::path part_path_for(const std::filesystem::path& current_path) {
  auto part_path = current_path;
  part_path += ".kikopart";
  return part_path;
}

std::uint64_t resumable_part_size(const std::filesystem::path& part_path, std::uint64_t declared_size) {
  std::error_code ec;
  if (!std::filesystem::exists(part_path, ec) || ec) return 0;
  auto have = static_cast<std::uint64_t>(std::filesystem::file_size(part_path, ec));
  if (ec || have > declared_size) return 0;
  return have;
}

bool hash_existing_part_prefix(const std::filesystem::path& part_path, std::uint64_t have, Bytes& buffer,
                               Sha256Hasher& hasher, std::string* prefix_sha256) {
  std::ifstream partial(part_path, std::ios::binary);
  if (!partial) return false;

  std::optional<Sha256Hasher> prefix_hasher;
  if (prefix_sha256 != nullptr) prefix_hasher.emplace();
  std::uint64_t hashed = 0;
  while (partial && hashed < have) {
    auto want = std::min<std::uint64_t>(buffer.size(), have - hashed);
    partial.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(want));
    auto got = partial.gcount();
    if (got <= 0) break;
    std::span<const std::uint8_t> chunk(buffer.data(), static_cast<std::size_t>(got));
    hasher.update(chunk);
    if (prefix_hasher) prefix_hasher->update(chunk);
    hashed += static_cast<std::uint64_t>(got);
  }
  if (hashed == have && prefix_hasher) *prefix_sha256 = hex_encode(prefix_hasher->finish());
  return hashed == have;
}

void verify_received_digest(const std::filesystem::path& part_path, const std::string& relative,
                            std::uint64_t received_size, std::uint64_t declared_size, const std::string& expected_sha256,
                            const std::string& actual_sha256) {
  if (received_size != declared_size) {
    std::error_code ec;
    std::filesystem::remove(part_path, ec);
    throw KikoError("received " + std::to_string(received_size) + " bytes for " + relative + ", expected " +
                    std::to_string(declared_size));
  }
  if (actual_sha256 != expected_sha256) {
    std::error_code ec;
    std::filesystem::remove(part_path, ec);
    throw KikoError("integrity check failed for " + relative + " (expected " + expected_sha256 + ", got " + actual_sha256 +
                    ")");
  }
}

namespace {

std::string sha256_file_hex(const std::filesystem::path& path, Bytes& buffer) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw KikoError("failed to open received file for verification: " + path.string());

  Sha256Hasher hasher;
  while (input) {
    input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    auto got = input.gcount();
    if (got <= 0) break;
    hasher.update(std::span<const std::uint8_t>(buffer.data(), static_cast<std::size_t>(got)));
  }
  return hex_encode(hasher.finish());
}

}  // namespace

void verify_part_file_digest(const std::filesystem::path& part_path, const std::string& relative,
                             std::uint64_t declared_size, const std::string& expected_sha256, Bytes& buffer) {
  std::error_code size_ec;
  auto received_size = std::filesystem::file_size(part_path, size_ec);
  if (size_ec) {
    std::error_code remove_ec;
    std::filesystem::remove(part_path, remove_ec);
    throw KikoError("received 0 bytes for " + relative + ", expected " + std::to_string(declared_size));
  }

  const auto actual_sha256 = sha256_file_hex(part_path, buffer);
  verify_received_digest(part_path, relative, static_cast<std::uint64_t>(received_size), declared_size, expected_sha256,
                         actual_sha256);
}

void finalize_part_file(const std::filesystem::path& part_path, const std::filesystem::path& current_path,
                        const std::string& relative) {
  std::error_code ec;
  std::filesystem::rename(part_path, current_path, ec);
  if (ec) {
    std::filesystem::remove(current_path, ec);
    std::filesystem::rename(part_path, current_path, ec);
    if (ec) throw KikoError("failed to finalize file: " + current_path.string());
  }
  (void)relative;
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

std::filesystem::path safe_join(const std::filesystem::path& base, const std::string& relative) {
  std::filesystem::path rel(relative);
  if (rel.is_absolute()) throw KikoError("refusing absolute path in transfer: " + relative);
  for (const auto& part : rel) {
    if (part == "..") throw KikoError("refusing path traversal in transfer: " + relative);
  }
  return base / rel;
}

}  // namespace kiko::detail

namespace kiko {

using namespace detail;

void send_files(TcpSocket& channel, const SessionKey& key, const std::vector<FileEntry>& files,
                ProgressReporter& reporter) {
  StreamCipher cipher(key, /*sender_originates=*/true);
  Bytes buffer(kPlainChunk);
  std::uint64_t grand_total = 0;

  for (const auto& entry : files) {
    if (is_dir_entry(entry)) {
      auto header = make_file_header(entry);
      send_tagged_text(channel, cipher, StreamTag::FileHeader, encode_message(header));
      reporter.file_start(entry.relative, 0);

      const auto resume = recv_resume_request(channel, cipher, entry);
      send_resume_ack(channel, cipher, resume.offset);

      Message end{"end", {{"sha256", kEmptySha256}}};
      send_tagged_text(channel, cipher, StreamTag::FileEnd, encode_message(end));
      reporter.file_complete(entry.relative, 0, false);
      continue;
    }

    std::ifstream input(entry.absolute, std::ios::binary);
    if (!input) throw KikoError("failed to open input file: " + entry.absolute.string());

    auto header = make_file_header(entry);
    send_tagged_text(channel, cipher, StreamTag::FileHeader, encode_message(header));
    reporter.file_start(entry.relative, entry.size);

    // The receiver replies with how many leading bytes it already has so we can
    // resume an interrupted transfer instead of resending the whole file.
    const auto resume = recv_resume_request(channel, cipher, entry);

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
    if (offset > 0) reporter.file_advance(offset);

    const bool use_zstd = should_compress_entry(entry);
    std::optional<ZstdStreamCompressor> compressor;
    if (use_zstd) compressor.emplace(3);
    while (input) {
      input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
      auto got = input.gcount();
      if (got <= 0) break;
      std::span<const std::uint8_t> chunk(buffer.data(), static_cast<std::size_t>(got));
      hasher->update(chunk);
      total += static_cast<std::uint64_t>(got);
      if (use_zstd) {
        auto compressed = compressor->compress(chunk, false);
        if (!compressed.empty()) send_tagged(channel, cipher, StreamTag::Data, compressed);
      } else {
        send_tagged(channel, cipher, StreamTag::Data, chunk);
      }
      reporter.file_advance(static_cast<std::uint64_t>(got));
    }
    if (use_zstd) {
      auto trailer = compressor->compress(std::span<const std::uint8_t>(), true);
      if (!trailer.empty()) send_tagged(channel, cipher, StreamTag::Data, trailer);
    }

    auto digest = hasher->finish();
    Message end{"end", {{"sha256", hex_encode(digest)}}};
    send_tagged_text(channel, cipher, StreamTag::FileEnd, encode_message(end));
    grand_total += total;
    reporter.file_complete(entry.relative, total, offset > 0);
  }

  send_tagged(channel, cipher, StreamTag::Done, std::span<const std::uint8_t>());
  reporter.transfer_complete(files.size(), grand_total);
}

void receive_files(TcpSocket& channel, const SessionKey& key, const std::filesystem::path& output_dir,
                   ProgressReporter& reporter) {
  StreamCipher cipher(key, /*sender_originates=*/false);
  Bytes buffer(kPlainChunk);

  std::ofstream out;
  std::optional<ZstdStreamDecompressor> decompressor;
  std::optional<Sha256Hasher> hasher;
  bool use_zstd = true;
  std::filesystem::path current_path;
  std::filesystem::path part_path;
  std::string current_relative;
  std::uint64_t current_total = 0;
  std::uint64_t current_declared_size = 0;
  bool resumed = false;
  bool skipping_file = false;
  bool is_dir_marker = false;
  std::uint64_t pending_mtime_ms = 0;
  std::size_t file_count = 0;
  std::uint64_t grand_total = 0;

  while (true) {
    auto frame = recv_tagged(channel, cipher);
    if (!frame) throw KikoError("transfer stream ended unexpectedly");

    switch (frame->tag) {
      case StreamTag::FileHeader: {
        auto header = decode_message(std::string(frame->payload.begin(), frame->payload.end()));
        current_relative = header.get("path");
        if (current_relative.empty()) throw KikoError("file header missing path");
        auto declared_size = header.get_u64("size", 0);
        current_declared_size = declared_size;
        use_zstd = header.get("compress", "zstd") != "none";
        current_path = safe_join(output_dir, current_relative);
        if (current_path.has_parent_path()) std::filesystem::create_directories(current_path.parent_path());

        if (is_dir_header(current_relative, declared_size)) {
          std::filesystem::create_directories(current_path);
          apply_file_mtime(current_path, header);
          send_resume(channel, cipher, 0);
          (void)recv_resume_ack(channel, cipher, 0, 0, current_relative);
          reporter.file_start(current_relative, 0);
          is_dir_marker = true;
          current_total = 0;
          current_declared_size = 0;
          pending_mtime_ms = 0;
          break;
        }

        pending_mtime_ms = header.get_u64("mtime_ms", 0);

        if (try_skip_existing_duplicate(channel, cipher, header, current_path, current_relative, declared_size, reporter)) {
          skipping_file = true;
          current_total = declared_size;
          break;
        }

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
        resumed = have > 0;
        reporter.file_start(current_relative, declared_size);
        if (have > 0) reporter.file_advance(have);
        break;
      }
      case StreamTag::Data: {
        if (skipping_file) break;
        if (!hasher) throw KikoError("data frame before file header");
        if (use_zstd) {
          if (!decompressor) throw KikoError("data frame before decompressor ready");
          auto decompressed = decompressor->decompress(
              frame->payload, declared_remaining_limit(current_total, current_declared_size, current_relative));
          if (!decompressed.empty()) {
            out.write(reinterpret_cast<const char*>(decompressed.data()), static_cast<std::streamsize>(decompressed.size()));
            hasher->update(decompressed);
            current_total += decompressed.size();
            reporter.file_advance(decompressed.size());
          }
        } else {
          if (!frame->payload.empty()) {
            ensure_declared_space(current_total, current_declared_size,
                                  static_cast<std::uint64_t>(frame->payload.size()), current_relative);
            out.write(reinterpret_cast<const char*>(frame->payload.data()),
                      static_cast<std::streamsize>(frame->payload.size()));
            hasher->update(frame->payload);
            current_total += frame->payload.size();
            reporter.file_advance(frame->payload.size());
          }
        }
        break;
      }
      case StreamTag::FileEnd: {
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
        apply_mtime_ms(current_path, pending_mtime_ms);
        pending_mtime_ms = 0;
        ++file_count;
        grand_total += current_total;
        reporter.file_complete(current_relative, current_total, true);
        if (resumed) reporter.status("resumed " + current_relative);
        current_total = 0;
        current_declared_size = 0;
        decompressor.reset();
        hasher.reset();
        break;
      }
      case StreamTag::Done:
        reporter.transfer_complete(file_count, grand_total);
        return;
      default:
        throw KikoError("unknown transfer frame tag");
    }
  }
}

}  // namespace kiko
