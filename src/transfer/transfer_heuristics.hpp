#pragma once

#include "core/common.hpp"
#include "transfer.hpp"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace kiko {

[[nodiscard]] bool should_compress_path(const std::filesystem::path& path);

// Suggest parallel relay connections from measured RTT and total payload size.
[[nodiscard]] int recommend_connections(std::int64_t relay_rtt_ms, std::uint64_t total_bytes);

struct TransferPayloadStats {
  double compressible_ratio = 0;
  std::uint64_t largest_file_bytes = 0;
};

// compressible_ratio = share of regular files (not dir markers) eligible for zstd.
[[nodiscard]] TransferPayloadStats transfer_payload_stats(const std::vector<FileEntry>& files);

}  // namespace kiko
