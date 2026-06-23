#pragma once

#include "adaptive.hpp"
#include "common.hpp"
#include "progress.hpp"
#include "proxy.hpp"
#include "socket.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace kiko {

class TransferCancellation;
struct FileEntry;
enum class ConflictPolicy;

void send_files_over_relay(TcpSocket relay_channel, const Endpoint& active_relay, const std::string& code,
                           int connections, const ConnectOptions& connect_options,
                           const std::optional<std::string>& relay_pass, const std::vector<FileEntry>& files,
                           ProgressReporter& reporter, RouteTiming timing = {},
                           TransferCancellation* cancellation = nullptr);

void receive_files_over_relay(TcpSocket relay_channel, const Endpoint& active_relay, const std::string& code,
                              int connections, const ConnectOptions& connect_options,
                              const std::optional<std::string>& relay_pass,
                              const std::filesystem::path& output_dir, ProgressReporter& reporter,
                              RouteTiming timing, ConflictPolicy conflict_policy,
                              TransferCancellation* cancellation = nullptr);

}  // namespace kiko
