#include "transfer_resume.hpp"

#include "file_metadata.hpp"
#include "imohash.hpp"
#include "transfer_stream.hpp"

#include <algorithm>
#include <fstream>
#include <optional>

namespace kiko::detail {

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

}  // namespace kiko::detail
