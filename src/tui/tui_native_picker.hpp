#pragma once

#include "platform/native_picker.hpp"

#include <string>

namespace kiko {

// Applies a system picker selection to a TUI text field. Cancellation leaves
// the current value unchanged; unavailable/error results populate menu_error.
[[nodiscard]] bool pick_tui_native_path(std::string& target, NativePickMode mode,
                                        std::string& menu_error);

}  // namespace kiko
