#pragma once

#include "common.hpp"
#include "crypto.hpp"
#include "file_metadata.hpp"
#include "protocol.hpp"
#include "receive_plan.hpp"
#include "socket.hpp"
#include "transfer.hpp"
#include "transfer_receive_paths.hpp"
#include "transfer_resume.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
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

void ensure_declared_space(std::uint64_t current_total, std::uint64_t declared_size, std::uint64_t next_size,
                           const std::string& relative);
[[nodiscard]] std::size_t declared_remaining_limit(std::uint64_t current_total, std::uint64_t declared_size,
                                                   const std::string& relative);

[[nodiscard]] bool is_dir_entry(const FileEntry& entry);
[[nodiscard]] bool is_symlink_entry(const FileEntry& entry);
void append_mtime_field(Message& header, const FileEntry& entry);
void append_mode_field(Message& header, const FileEntry& entry);
[[nodiscard]] bool should_compress_entry(const FileEntry& entry);
[[nodiscard]] Message make_file_header(const FileEntry& entry);
void send_transfer_manifest(TcpSocket& socket, StreamCipher& cipher, const std::vector<FileEntry>& files);

void send_tagged(TcpSocket& socket, StreamCipher& cipher, StreamTag tag, std::span<const std::uint8_t> payload);
void send_tagged_text(TcpSocket& socket, StreamCipher& cipher, StreamTag tag, const std::string& text);
[[nodiscard]] std::optional<TaggedFrame> recv_tagged(TcpSocket& socket, StreamCipher& cipher);

using TransferClock = std::chrono::steady_clock;

[[nodiscard]] std::int64_t transfer_elapsed_ms_since(TransferClock::time_point start);
void add_transfer_elapsed(std::int64_t& bucket, TransferClock::time_point start);
void send_tagged_timed(TcpSocket& socket, StreamCipher& cipher, StreamTag tag,
                       std::span<const std::uint8_t> payload, TransferTiming& timing);
void send_tagged_text_timed(TcpSocket& socket, StreamCipher& cipher, StreamTag tag, const std::string& text,
                            TransferTiming& timing);

}  // namespace kiko::detail
