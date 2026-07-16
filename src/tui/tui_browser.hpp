#pragma once

#include "platform/path_browser.hpp"

#include <filesystem>
#include <optional>

namespace kiko {

// Fullscreen path picker. Returns nullopt if the user cancels (Esc/q).
[[nodiscard]] std::optional<std::filesystem::path> run_tui_path_picker(const std::filesystem::path& start,
                                                                       PathPickMode mode);

}  // namespace kiko
