#include "transfer_stream.hpp"

#include "compression.hpp"
#include "imohash.hpp"
#include "transfer_heuristics.hpp"

#include <nlohmann/json.hpp>

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

bool is_symlink_entry(const FileEntry& entry) {
  return entry.symlink;
}

bool is_dir_header(const std::string& path, std::uint64_t size) {
  return size == 0 && !path.empty() && path.back() == '/';
}

bool is_symlink_header(const Message& header) {
  return header.type == "symlink" || header.get("kind") == "symlink";
}

bool target_is_safe_relative_symlink(const std::filesystem::path& target) {
  if (target.empty() || target.is_absolute()) return false;
  for (const auto& part : target) {
    if (part == "..") return false;
  }
  return true;
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

void send_resume(TcpSocket& socket, StreamCipher& cipher, std::uint64_t offset, const std::string& prefix_sha256,
                 bool complete_skip) {
  Message resume{"resume", {{"offset", std::to_string(offset)}}};
  if (!prefix_sha256.empty()) resume.fields["prefix_sha256"] = prefix_sha256;
  if (complete_skip) resume.fields["complete_skip"] = "1";
  send_tagged_text(socket, cipher, StreamTag::Resume, encode_message(resume));
}

ResumeRequest recv_resume_request(TcpSocket& socket, StreamCipher& cipher, const FileEntry& entry) {
  auto resume = recv_tagged(socket, cipher);
  if (!resume || resume->tag != StreamTag::Resume) throw KikoError("expected resume frame");
  auto resume_msg = decode_message(std::string(resume->payload.begin(), resume->payload.end()));
  auto offset = resume_msg.get_u64("offset", 0);
  if (offset > entry.size) offset = 0;
  const bool complete_skip = offset == entry.size && resume_msg.get("complete_skip") == "1";
  return ResumeRequest{offset, resume_msg.get("prefix_sha256"), complete_skip};
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

  send_resume(socket, cipher, declared_size, {}, true);
  const auto accepted = recv_resume_ack(socket, cipher, declared_size, declared_size, current_relative);
  if (accepted != declared_size) return false;
  apply_file_metadata(current_path, header);
  reporter.status("skipped duplicate " + current_relative);
  reporter.file_start(current_relative, declared_size);
  reporter.file_advance(declared_size);
  return true;
}

bool path_exists_no_follow(const std::filesystem::path& path) {
  std::error_code ec;
  const auto status = std::filesystem::symlink_status(path, ec);
  return !ec && std::filesystem::exists(status);
}

std::filesystem::path unique_conflict_path(const std::filesystem::path& path) {
  const auto parent = path.parent_path();
  auto stem = path.stem().string();
  const auto extension = path.extension().string();
  if (stem.empty()) stem = path.filename().string();
  if (stem.empty()) stem = "received";

  for (int i = 1; i < 10000; ++i) {
    auto candidate = parent / (stem + " (" + std::to_string(i) + ")" + extension);
    auto candidate_part = candidate;
    candidate_part += ".kikopart";
    if (!path_exists_no_follow(candidate) && !path_exists_no_follow(candidate_part)) return candidate;
  }
  throw KikoError("could not choose a non-conflicting filename for " + path.string());
}

void report_renamed_conflict(const std::string& relative, const std::filesystem::path& renamed,
                             ProgressReporter& reporter) {
  reporter.status("renamed conflict " + relative + " -> " + renamed.filename().generic_string());
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

void validate_safe_symlink_target(const std::string& relative, const std::string& target) {
  if (relative.empty() || relative.back() == '/') {
    throw KikoError("refusing invalid symlink path: " + relative);
  }
  const std::filesystem::path link_target(target);
  if (!target_is_safe_relative_symlink(link_target)) {
    throw KikoError("refusing unsafe symlink target for " + relative + ": " + target);
  }
}

void create_safe_symlink(const std::filesystem::path& current_path, const std::string& relative,
                         const std::string& target) {
  validate_safe_symlink_target(relative, target);
  const std::filesystem::path link_target(target);
  std::error_code ec;
  if (current_path.has_parent_path()) std::filesystem::create_directories(current_path.parent_path());
  std::filesystem::remove(current_path, ec);
  ec.clear();
  std::filesystem::create_symlink(link_target, current_path, ec);
  if (ec) throw KikoError("failed to create symlink: " + current_path.string());
}

namespace {

std::string manifest_kind(const FileEntry& entry) {
  if (is_symlink_entry(entry)) return "symlink";
  if (is_dir_entry(entry)) return "dir";
  return "file";
}

std::string manifest_string_field(const nlohmann::json& object, const char* key, const std::string& fallback = {}) {
  if (!object.contains(key)) return fallback;
  const auto& value = object.at(key);
  if (!value.is_string()) throw KikoError(std::string("manifest field must be a string: ") + key);
  return value.get<std::string>();
}

std::uint64_t manifest_u64_field(const nlohmann::json& object, const char* key, std::uint64_t fallback = 0) {
  if (!object.contains(key)) return fallback;
  const auto& value = object.at(key);
  if (value.is_number_unsigned()) return value.get<std::uint64_t>();
  if (value.is_number_integer()) {
    const auto signed_value = value.get<std::int64_t>();
    if (signed_value < 0) throw KikoError(std::string("manifest field must be unsigned: ") + key);
    return static_cast<std::uint64_t>(signed_value);
  }
  if (value.is_string()) {
    auto parsed = parse_u64_strict(value.get<std::string>());
    if (parsed) return *parsed;
  }
  throw KikoError(std::string("manifest field must be an unsigned integer: ") + key);
}

std::uint32_t manifest_u32_field(const nlohmann::json& object, const char* key, std::uint32_t fallback = 0) {
  const auto value = manifest_u64_field(object, key, fallback);
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    throw KikoError(std::string("manifest field is too large: ") + key);
  }
  return static_cast<std::uint32_t>(value);
}

void ensure_manifest_total(std::uint64_t& total, std::uint64_t size, const std::string& relative) {
  if (size > std::numeric_limits<std::uint64_t>::max() - total) {
    throw KikoError("manifest total size overflow near " + relative);
  }
  total += size;
}

}  // namespace

std::string encode_transfer_manifest(const std::vector<FileEntry>& files) {
  nlohmann::json root = nlohmann::json::object();
  root["version"] = 1;
  root["entries"] = nlohmann::json::array();

  std::uint64_t total_size = 0;
  for (const auto& entry : files) {
    nlohmann::json item = nlohmann::json::object();
    item["path"] = entry.relative;
    item["kind"] = manifest_kind(entry);
    item["size"] = entry.size;
    if (!entry.imohash.empty()) item["imohash"] = entry.imohash;
    if (!entry.link_target.empty()) item["target"] = entry.link_target;
    if (entry.mtime_ms > 0) item["mtime_ms"] = entry.mtime_ms;
    if (entry.mode > 0) item["mode"] = entry.mode;
    root["entries"].push_back(std::move(item));
    if (!is_dir_entry(entry) && !is_symlink_entry(entry)) ensure_manifest_total(total_size, entry.size, entry.relative);
  }

  root["count"] = files.size();
  root["total_size"] = total_size;
  return root.dump();
}

TransferManifest decode_transfer_manifest(std::string_view text) {
  nlohmann::json root;
  try {
    root = nlohmann::json::parse(std::string(text));
  } catch (const nlohmann::json::exception& error) {
    throw KikoError(std::string("invalid transfer manifest json: ") + error.what());
  }
  if (!root.is_object()) throw KikoError("transfer manifest must be a json object");
  const auto version = manifest_u64_field(root, "version", 1);
  if (version != 1) throw KikoError("unsupported transfer manifest version: " + std::to_string(version));
  if (!root.contains("entries") || !root["entries"].is_array()) {
    throw KikoError("transfer manifest missing entries");
  }

  TransferManifest manifest;
  std::uint64_t computed_total = 0;
  for (const auto& item : root["entries"]) {
    if (!item.is_object()) throw KikoError("transfer manifest entry must be a json object");
    TransferManifestEntry entry;
    entry.path = manifest_string_field(item, "path");
    entry.kind = manifest_string_field(item, "kind", "file");
    entry.size = manifest_u64_field(item, "size", 0);
    entry.target = manifest_string_field(item, "target");
    entry.imohash = manifest_string_field(item, "imohash");
    entry.mtime_ms = manifest_u64_field(item, "mtime_ms", 0);
    entry.mode = manifest_u32_field(item, "mode", 0);
    if (entry.kind == "file") ensure_manifest_total(computed_total, entry.size, entry.path);
    manifest.entries.push_back(std::move(entry));
  }

  const auto declared_count = manifest_u64_field(root, "count", manifest.entries.size());
  if (declared_count != manifest.entries.size()) {
    throw KikoError("transfer manifest count mismatch");
  }
  const auto declared_total = manifest_u64_field(root, "total_size", computed_total);
  if (declared_total != computed_total) {
    throw KikoError("transfer manifest total_size mismatch");
  }
  manifest.total_size = computed_total;
  return manifest;
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
  send_transfer_manifest(channel, cipher, files);

  for (const auto& entry : files) {
    if (is_symlink_entry(entry)) {
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
    if (resume.complete_skip && resume.offset == entry.size) {
      send_resume_ack(channel, cipher, entry.size);
      reporter.status("skipped already-complete " + entry.relative);
      reporter.file_resume(entry.relative, entry.size, entry.size);
      reporter.file_advance(entry.size);
      Message end{"end", {{"sha256", kEmptySha256}}};
      send_tagged_text(channel, cipher, StreamTag::FileEnd, encode_message(end));
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
    reporter.file_complete(entry.relative, total, false);
  }

  send_tagged(channel, cipher, StreamTag::Done, std::span<const std::uint8_t>());
  auto ack = recv_tagged(channel, cipher);
  if (!ack || ack->tag != StreamTag::Ack) throw KikoError("expected transfer ack");
  reporter.transfer_complete(files.size(), grand_total);
}

void receive_files(TcpSocket& channel, const SessionKey& key, const std::filesystem::path& output_dir,
                   ProgressReporter& reporter, ConflictPolicy conflict_policy) {
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
        send_tagged(channel, cipher, StreamTag::Ack, std::span<const std::uint8_t>());
        reporter.transfer_complete(file_count, grand_total);
        return;
      default:
        throw KikoError("unknown transfer frame tag");
    }
  }
}

}  // namespace kiko
