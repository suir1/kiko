#pragma once

#include <filesystem>
#include <optional>

namespace kiko {

enum class TuiPickMode { FileOrDirectory, DirectoryOnly };

// Fullscreen path picker. Returns nullopt if the user cancels (Esc/q).
[[nodiscard]] std::optional<std::filesystem::path> run_tui_path_picker(const std::filesystem::path& start,
                                                                       TuiPickMode mode);

}  // namespace kiko
