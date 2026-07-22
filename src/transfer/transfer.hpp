#pragma once

#include "connect/peer_options.hpp"
#include "core/crypto.hpp"
#include "core/progress.hpp"
#include "core/socket.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace kiko {

enum class SymlinkMode {
  Follow,
  Preserve,
};

enum class ConflictPolicy {
  Overwrite,
  Skip,
  Rename,
};

struct TransferSessionConfig : PeerConnectionOptions {
  std::string code;
  bool ai_route = false;
  bool ai_route_plan_only = false;
  bool auto_reconnect = true;
  int reconnect_attempts = 3;
  std::chrono::milliseconds reconnect_delay{1000};
  bool debug_route = false;
};

struct SendConfig : TransferSessionConfig {
  std::filesystem::path file;
  bool use_gitignore = true;
  bool show_qrcode = true;
  bool ai_route_connectivity_only = false;
  bool auto_connections = false;
  SymlinkMode symlink_mode = SymlinkMode::Follow;
  // Number of parallel TCP connections to use on the relay path (1 = single
  // stream). The sender controls this; the receiver mirrors it.
  int connections = 4;
};

struct FileEntry {
  std::filesystem::path absolute;
  std::string relative;
  std::uint64_t size = 0;
  std::string imohash;
  std::uint64_t mtime_ms = 0;  // unix epoch ms; 0 = omit from wire
  std::uint32_t mode = 0;      // POSIX execute bits to preserve; 0 = omit from wire
  bool symlink = false;
  std::string link_target;
};

enum class TransferEntryKind {
  File,
  Directory,
  Symlink,
};

[[nodiscard]] inline TransferEntryKind transfer_entry_kind(const FileEntry& entry) {
  if (entry.symlink) return TransferEntryKind::Symlink;
  if (entry.size == 0 && !entry.relative.empty() && entry.relative.back() == '/') {
    return TransferEntryKind::Directory;
  }
  return TransferEntryKind::File;
}

[[nodiscard]] inline const char* transfer_entry_kind_name(TransferEntryKind kind) {
  switch (kind) {
    case TransferEntryKind::File:
      return "file";
    case TransferEntryKind::Directory:
      return "dir";
    case TransferEntryKind::Symlink:
      return "symlink";
  }
  return "file";
}

struct CollectOptions {
  bool use_gitignore = true;
  SymlinkMode symlink_mode = SymlinkMode::Follow;
};

// Enumerates the files to send. A directory is walked recursively with the
// top-level directory name preserved in each relative path.
[[nodiscard]] std::vector<FileEntry> collect_files(const std::filesystem::path& path,
                                                   const CollectOptions& options = {});

// Post-handshake transfer primitives over an established, key-agreed channel.
// A manifest preflight is followed by per-file headers, zstd-compressed
// XChaCha20-Poly1305 data chunks, and SHA-256 trailers for integrity.
void send_files(TcpSocket& channel, const SessionKey& key, const std::vector<FileEntry>& files,
                ProgressReporter& reporter);
void receive_files(TcpSocket& channel, const SessionKey& key, const std::filesystem::path& output_dir,
                   ProgressReporter& reporter, ConflictPolicy conflict_policy = ConflictPolicy::Overwrite);

// Multiplexed variants spreading file chunks across several connections that
// share one session key (each channel uses a distinct cipher stream id). Channel
// 0 is the control channel carrying headers/trailers; data flows on all of them.
void send_files_mux(std::vector<TcpSocket>& channels, const SessionKey& key, const std::vector<FileEntry>& files,
                    ProgressReporter& reporter);
void receive_files_mux(std::vector<TcpSocket>& channels, const SessionKey& key, const std::filesystem::path& output_dir,
                       ProgressReporter& reporter, ConflictPolicy conflict_policy = ConflictPolicy::Overwrite);

struct RecvConfig : TransferSessionConfig {
  std::filesystem::path output_dir{"."};
  ConflictPolicy conflict_policy = ConflictPolicy::Overwrite;
};

int run_send(const SendConfig& config, ProgressReporter& reporter);
int run_recv(const RecvConfig& config, ProgressReporter& reporter);

}  // namespace kiko
