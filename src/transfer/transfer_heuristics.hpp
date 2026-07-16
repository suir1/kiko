#pragma once

#include <cstdint>
#include <filesystem>

namespace kiko {

[[nodiscard]] bool should_compress_path(const std::filesystem::path& path);

// Suggest parallel relay connections from measured RTT and total payload size.
[[nodiscard]] int recommend_connections(std::int64_t relay_rtt_ms, std::uint64_t total_bytes);

}  // namespace kiko
