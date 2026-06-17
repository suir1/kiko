#pragma once

#include "protocol.hpp"

#include <cstdint>
#include <filesystem>

namespace kiko::detail {

[[nodiscard]] std::uint64_t file_mtime_ms(const std::filesystem::path& path);
void apply_mtime_ms(const std::filesystem::path& path, std::uint64_t mtime_ms);
void apply_file_mtime(const std::filesystem::path& path, const Message& header);

}  // namespace kiko::detail
