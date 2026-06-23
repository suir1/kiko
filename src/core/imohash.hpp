#pragma once

#include <filesystem>
#include <string>

namespace kiko {

// Fast sampled fingerprint (imohash-style) for resume/dedup checks.
[[nodiscard]] std::string imohash_hex(const std::filesystem::path& path);

}  // namespace kiko
