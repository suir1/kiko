#pragma once

#include <iostream>
#include <optional>
#include <string>

namespace kiko {

void print_qrcode(std::ostream& out, const std::string& text);
[[nodiscard]] std::optional<std::string> qrcode_svg(const std::string& text);

}  // namespace kiko
