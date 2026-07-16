#pragma once

#include "core/crypto.hpp"
#include "core/progress.hpp"
#include "core/protocol.hpp"
#include "core/socket.hpp"
#include "receive_plan.hpp"
#include "transfer.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>

namespace kiko::detail {

[[nodiscard]] bool is_dir_entry(const FileEntry& entry);

enum class SendFileAction {
  MarkerComplete,
  SkipPayload,
  SendPayload,
};

struct SendFileChunk {
  std::uint64_t offset = 0;
  std::span<const std::uint8_t> bytes;
};

class SendFileSession {
 public:
  SendFileSession(const FileEntry& entry, TcpSocket& control, StreamCipher& cipher, ProgressReporter& reporter,
                  TransferTiming& timing, Bytes& buffer);

  [[nodiscard]] std::optional<SendFileChunk> read_next(Bytes& buffer);
  [[nodiscard]] std::uint64_t complete();
  [[nodiscard]] std::uint64_t complete_skipped();

  [[nodiscard]] SendFileAction action() const { return action_; }
  [[nodiscard]] bool use_zstd() const { return use_zstd_; }

 private:
  [[nodiscard]] std::uint64_t finish(const std::string& sha256, std::uint64_t total);

  const FileEntry& entry_;
  TcpSocket& control_;
  StreamCipher& cipher_;
  ProgressReporter& reporter_;
  TransferTiming& timing_;
  std::ifstream input_;
  std::optional<Sha256Hasher> hasher_;
  SendFileAction action_ = SendFileAction::SendPayload;
  std::uint64_t total_ = 0;
  bool use_zstd_ = false;
};

enum class ReceiveFileAction {
  Marker,
  SkipPayload,
  ReceivePayload,
};

class ReceiveFileSession {
 public:
  ReceiveFileSession(Message header, std::filesystem::path output_dir, ConflictPolicy conflict_policy,
                     const ReceivePlan* receive_plan, TcpSocket& control, StreamCipher& cipher,
                     ProgressReporter& reporter, Bytes& prefix_buffer);

  void complete_marker();
  [[nodiscard]] std::uint64_t complete_skipped();
  [[nodiscard]] std::uint64_t complete_received(const std::string& expected_sha256,
                                                std::uint64_t received_size,
                                                const std::string& actual_sha256,
                                                Bytes& verify_buffer);

  [[nodiscard]] ReceiveFileAction action() const { return action_; }
  [[nodiscard]] const std::string& relative() const { return relative_; }
  [[nodiscard]] const std::filesystem::path& part_path() const { return part_path_; }
  [[nodiscard]] std::uint64_t declared_size() const { return declared_size_; }
  [[nodiscard]] std::uint64_t resume_offset() const { return resume_offset_; }
  [[nodiscard]] bool use_zstd() const { return use_zstd_; }
  [[nodiscard]] Sha256Hasher& hasher();

 private:
  Message header_;
  TcpSocket& control_;
  StreamCipher& cipher_;
  ProgressReporter& reporter_;
  std::filesystem::path current_path_;
  std::filesystem::path part_path_;
  std::string relative_;
  std::string symlink_target_;
  std::optional<Sha256Hasher> hasher_;
  std::uint64_t declared_size_ = 0;
  std::uint64_t resume_offset_ = 0;
  ReceiveFileAction action_ = ReceiveFileAction::ReceivePayload;
  bool use_zstd_ = false;
  bool symlink_marker_ = false;
  bool skip_symlink_ = false;
};

}  // namespace kiko::detail
