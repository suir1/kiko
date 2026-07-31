#include "core/random.hpp"

#include "core/crypto.hpp"

#include <sodium.h>

#include <limits>

namespace kiko {
namespace {

void validate_output_size(std::size_t bytes) {
  if (bytes > std::numeric_limits<std::size_t>::max() / 2) {
    throw KikoError("secure random output length is too large");
  }
}

}  // namespace

std::string secure_random_hex(std::size_t bytes) {
  validate_output_size(bytes);
  ensure_sodium_ready();
  Bytes random(bytes);
  if (!random.empty()) randombytes_buf(random.data(), random.size());
  return hex_encode(random);
}

std::string random_code(std::size_t bytes) {
  static constexpr char alphabet[] = "23456789abcdefghijkmnpqrstuvwxyz";
  validate_output_size(bytes);
  ensure_sodium_ready();

  std::string out(bytes * 2, '\0');
  for (auto& value : out) {
    value = alphabet[randombytes_uniform(static_cast<std::uint32_t>(sizeof(alphabet) - 1))];
  }
  return out;
}

}  // namespace kiko
