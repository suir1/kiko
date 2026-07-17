#pragma once

#include "core/common.hpp"
#include "core/crypto.hpp"
#include "core/progress.hpp"
#include "core/protocol.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace kiko::detail {

[[nodiscard]] bool is_safe_relative_symlink_target(const std::filesystem::path& target);
[[nodiscard]] bool is_dir_header(const std::string& path, std::uint64_t size);
[[nodiscard]] bool is_symlink_header(const Message& header);
[[nodiscard]] bool path_exists_no_follow(const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path unique_conflict_path(
    const std::filesystem::path& path,
    const std::function<bool(const std::filesystem::path&)>& reserved = {});
void report_renamed_conflict(const std::string& relative, const std::filesystem::path& renamed,
                             ProgressReporter& reporter);
void verify_received_digest(const std::filesystem::path& part_path, const std::string& relative,
                            std::uint64_t received_size, std::uint64_t declared_size, const std::string& expected_sha256,
                            const std::string& actual_sha256);
void verify_part_file_digest(const std::filesystem::path& part_path, const std::string& relative,
                             std::uint64_t declared_size, const std::string& expected_sha256, Bytes& buffer);
void finalize_part_file(const std::filesystem::path& part_path, const std::filesystem::path& current_path);
void validate_safe_symlink_target(const std::string& relative, const std::string& target);
void create_safe_symlink(const std::filesystem::path& current_path, const std::string& relative,
                         const std::string& target);
[[nodiscard]] std::filesystem::path safe_join(const std::filesystem::path& base, const std::string& relative);

}  // namespace kiko::detail
