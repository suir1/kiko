#pragma once

#include <cstddef>
#include <string>

namespace kiko {

[[nodiscard]] std::string secure_random_hex(std::size_t bytes);
// Short pairing code: `bytes * 2` chars from an unambiguous alphabet (default 6).
[[nodiscard]] std::string random_code(std::size_t bytes = 3);

}  // namespace kiko
