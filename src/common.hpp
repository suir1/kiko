#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace kiko {

using Bytes = std::vector<std::uint8_t>;

class KikoError : public std::runtime_error {
 public:
  explicit KikoError(const std::string& message) : std::runtime_error(message) {}
};

struct Endpoint {
  std::string host;
  std::uint16_t port = 0;

  [[nodiscard]] std::string to_string() const {
    if (host.find(':') != std::string::npos && (host.empty() || host.front() != '[')) {
      return "[" + host + "]:" + std::to_string(port);
    }
    return host + ":" + std::to_string(port);
  }
};

Endpoint parse_endpoint(const std::string& value, std::uint16_t default_port = 0);
Endpoint parse_bind_endpoint(const std::string& value, std::uint16_t default_port = 0);

enum class IpAddressFamily { Unknown, IPv4, IPv6 };
enum class IpAddressScope { Unknown, Loopback, LinkLocal, Private, UniqueLocal, Global };

[[nodiscard]] IpAddressFamily ip_address_family(std::string_view host);
[[nodiscard]] IpAddressScope ip_address_scope(std::string_view host);
[[nodiscard]] bool is_ipv6_address(std::string_view host);
[[nodiscard]] bool is_global_ipv6_address(std::string_view host);
[[nodiscard]] std::size_t count_global_ipv6_addresses(const std::vector<std::string>& hosts);

// Short pairing code: `bytes * 2` chars from an unambiguous alphabet (default 6).
std::string random_code(std::size_t bytes = 3);

// Optional croc-style mnemonic code for --code: numeric prefix + words, e.g.
// "4827-stone-iris-lake-ruby" (prefix = rendezvous label; words = PAKE secret).
std::string random_mnemonic_code(std::size_t words = 4);
// Returns an error message when the pairing code format is invalid.
[[nodiscard]] std::optional<std::string> validate_pairing_code_format(const std::string& code, bool required);
std::optional<std::uint64_t> parse_u64_strict(const std::string& value);
std::vector<std::string> split_csv(const std::string& value);
std::string join_csv(const std::vector<std::string>& values);
std::string hex_encode(const Bytes& bytes);
Bytes hex_decode(const std::string& hex);
std::string trim(const std::string& value);
std::uint64_t now_ms();

}  // namespace kiko
