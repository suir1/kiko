#pragma once

#include <string>

namespace kiko {

[[nodiscard]] bool copy_to_clipboard(const std::string& text);

}  // namespace kiko
