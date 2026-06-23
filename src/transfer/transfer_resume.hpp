#pragma once

#include "core/common.hpp"
#include "core/crypto.hpp"
#include "core/progress.hpp"
#include "core/protocol.hpp"
#include "core/socket.hpp"
#include "transfer.hpp"

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>

namespace kiko::detail {

struct ResumeRequest {
  std::uint64_t offset = 0;
  std::string prefix_sha256;
  bool complete_skip = false;
};

void send_resume(TcpSocket& socket, StreamCipher& cipher, std::uint64_t offset,
                 const std::string& prefix_sha256 = {}, bool complete_skip = false);
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
[[nodiscard]] std::filesystem::path part_path_for(const std::filesystem::path& current_path);
[[nodiscard]] std::uint64_t resumable_part_size(const std::filesystem::path& part_path, std::uint64_t declared_size);
[[nodiscard]] bool hash_existing_part_prefix(const std::filesystem::path& part_path, std::uint64_t have, Bytes& buffer,
                                             Sha256Hasher& hasher, std::string* prefix_sha256 = nullptr);

}  // namespace kiko::detail
