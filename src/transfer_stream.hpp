#pragma once

#include "common.hpp"
#include "crypto.hpp"
#include "file_metadata.hpp"
#include "protocol.hpp"
#include "socket.hpp"
#include "transfer.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace kiko::detail {

constexpr std::size_t kPlainChunk = 128 * 1024;
constexpr std::size_t kMuxChunk = 256 * 1024;
constexpr int kMaxMuxConnections = 32;
inline constexpr const char* kEmptySha256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

enum class StreamTag : std::uint8_t {
  FileHeader = 1,
  Data = 2,
  FileEnd = 3,
  Done = 4,
  Resume = 5,
  ChunkEnd = 6,
  Ack = 7,
  ResumeAck = 8,
  Manifest = 9,
};

struct TaggedFrame {
  StreamTag tag;
  Bytes payload;
};

struct ResumeRequest {
  std::uint64_t offset = 0;
  std::string prefix_sha256;
};

struct TransferManifestEntry {
  std::string path;
  std::uint64_t size = 0;
  std::string kind;
  std::string target;
  std::string imohash;
  std::uint64_t mtime_ms = 0;
  std::uint32_t mode = 0;
};

struct TransferManifest {
  std::vector<TransferManifestEntry> entries;
  std::uint64_t total_size = 0;
};

void ensure_declared_space(std::uint64_t current_total, std::uint64_t declared_size, std::uint64_t next_size,
                           const std::string& relative);
[[nodiscard]] std::size_t declared_remaining_limit(std::uint64_t current_total, std::uint64_t declared_size,
                                                   const std::string& relative);

[[nodiscard]] bool is_dir_entry(const FileEntry& entry);
[[nodiscard]] bool is_symlink_entry(const FileEntry& entry);
[[nodiscard]] bool is_dir_header(const std::string& path, std::uint64_t size);
[[nodiscard]] bool is_symlink_header(const Message& header);
void append_mtime_field(Message& header, const FileEntry& entry);
void append_mode_field(Message& header, const FileEntry& entry);
[[nodiscard]] bool should_compress_entry(const FileEntry& entry);
[[nodiscard]] Message make_file_header(const FileEntry& entry);
void send_resume(TcpSocket& socket, StreamCipher& cipher, std::uint64_t offset, const std::string& prefix_sha256 = {});
[[nodiscard]] ResumeRequest recv_resume_request(TcpSocket& socket, StreamCipher& cipher, const FileEntry& entry);
void send_resume_ack(TcpSocket& socket, StreamCipher& cipher, std::uint64_t accepted_offset);
[[nodiscard]] std::uint64_t recv_resume_ack(TcpSocket& socket, StreamCipher& cipher, std::uint64_t requested_offset,
                                            std::uint64_t declared_size, const std::string& relative);
[[nodiscard]] std::uint64_t hash_stream_prefix(std::istream& input, Bytes& buffer, Sha256Hasher& hasher,
                                               std::uint64_t offset, const std::string& relative,
                                               std::string* prefix_sha256 = nullptr);
[[nodiscard]] bool try_skip_existing_duplicate(TcpSocket& socket, StreamCipher& cipher, const Message& header,
                                               const std::filesystem::path& current_path,
                                               const std::string& current_relative, std::uint64_t declared_size,
                                               ProgressReporter& reporter);
[[nodiscard]] bool path_exists_no_follow(const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path unique_conflict_path(const std::filesystem::path& path);
void report_renamed_conflict(const std::string& relative, const std::filesystem::path& renamed,
                             ProgressReporter& reporter);
[[nodiscard]] std::filesystem::path part_path_for(const std::filesystem::path& current_path);
[[nodiscard]] std::uint64_t resumable_part_size(const std::filesystem::path& part_path, std::uint64_t declared_size);
[[nodiscard]] bool hash_existing_part_prefix(const std::filesystem::path& part_path, std::uint64_t have, Bytes& buffer,
                                             Sha256Hasher& hasher, std::string* prefix_sha256 = nullptr);
void verify_received_digest(const std::filesystem::path& part_path, const std::string& relative,
                            std::uint64_t received_size, std::uint64_t declared_size, const std::string& expected_sha256,
                            const std::string& actual_sha256);
void verify_part_file_digest(const std::filesystem::path& part_path, const std::string& relative,
                             std::uint64_t declared_size, const std::string& expected_sha256, Bytes& buffer);
void finalize_part_file(const std::filesystem::path& part_path, const std::filesystem::path& current_path,
                        const std::string& relative);
void validate_safe_symlink_target(const std::string& relative, const std::string& target);
void create_safe_symlink(const std::filesystem::path& current_path, const std::string& relative,
                         const std::string& target);
[[nodiscard]] std::string encode_transfer_manifest(const std::vector<FileEntry>& files);
[[nodiscard]] TransferManifest decode_transfer_manifest(std::string_view text);
void send_transfer_manifest(TcpSocket& socket, StreamCipher& cipher, const std::vector<FileEntry>& files);
void preflight_transfer_manifest(const TransferManifest& manifest, const std::filesystem::path& output_dir,
                                 ConflictPolicy conflict_policy, ProgressReporter& reporter);

void send_tagged(TcpSocket& socket, StreamCipher& cipher, StreamTag tag, std::span<const std::uint8_t> payload);
void send_tagged_text(TcpSocket& socket, StreamCipher& cipher, StreamTag tag, const std::string& text);
[[nodiscard]] std::optional<TaggedFrame> recv_tagged(TcpSocket& socket, StreamCipher& cipher);

[[nodiscard]] std::filesystem::path safe_join(const std::filesystem::path& base, const std::string& relative);

}  // namespace kiko::detail
