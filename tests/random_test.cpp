#include "core/common.hpp"
#include "core/random.hpp"

#include <cassert>
#include <iostream>
#include <limits>
#include <set>
#include <string>
#include <string_view>

using namespace kiko;

int main() {
  constexpr std::string_view code_alphabet = "23456789abcdefghijkmnpqrstuvwxyz";
  constexpr std::string_view hex_alphabet = "0123456789abcdef";

  assert(secure_random_hex(0).empty());
  assert(random_code(0).empty());

  std::set<std::string> hex_values;
  std::set<std::string> code_values;
  for (int i = 0; i < 32; ++i) {
    const auto hex = secure_random_hex(16);
    const auto code = random_code(8);
    assert(hex.size() == 32);
    assert(code.size() == 16);
    for (const char value : hex) assert(hex_alphabet.find(value) != std::string_view::npos);
    for (const char value : code) assert(code_alphabet.find(value) != std::string_view::npos);
    hex_values.insert(hex);
    code_values.insert(code);
  }
  assert(hex_values.size() == 32);
  assert(code_values.size() == 32);

  bool rejected_hex_length = false;
  try {
    (void)secure_random_hex(std::numeric_limits<std::size_t>::max());
  } catch (const KikoError&) {
    rejected_hex_length = true;
  }
  assert(rejected_hex_length);

  bool rejected_code_length = false;
  try {
    (void)random_code(std::numeric_limits<std::size_t>::max());
  } catch (const KikoError&) {
    rejected_code_length = true;
  }
  assert(rejected_code_length);

  std::cout << "random ok\n";
  return 0;
}
