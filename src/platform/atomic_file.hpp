#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace kiko {

[[nodiscard]] std::optional<std::string> atomic_replace_file(const std::filesystem::path& source,
                                                             const std::filesystem::path& destination);
[[nodiscard]] std::optional<std::string> atomic_write_text_file(const std::filesystem::path& path,
                                                                std::string_view contents);
[[nodiscard]] std::optional<std::string> move_file_to_recovery_backup(const std::filesystem::path& path,
                                                                      std::string_view tag,
                                                                      std::filesystem::path& backup_path);

}  // namespace kiko
