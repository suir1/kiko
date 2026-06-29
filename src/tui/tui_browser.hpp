#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace kiko {

enum class TuiPickMode { FileOrDirectory, DirectoryOnly };
enum class TuiBrowserSort { Name, ModifiedDesc };

namespace detail {

[[nodiscard]] std::vector<std::string> list_tui_path_picker_labels(const std::filesystem::path& dir,
                                                                   TuiPickMode mode);
[[nodiscard]] std::vector<std::string> list_tui_path_picker_labels(const std::filesystem::path& dir,
                                                                   TuiPickMode mode, TuiBrowserSort sort);

}  // namespace detail

// Fullscreen path picker. Returns nullopt if the user cancels (Esc/q).
[[nodiscard]] std::optional<std::filesystem::path> run_tui_path_picker(const std::filesystem::path& start,
                                                                       TuiPickMode mode);

}  // namespace kiko
