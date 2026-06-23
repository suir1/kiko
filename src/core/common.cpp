#include "core/common.hpp"

#include "core/wordlist.hpp"

#include <asio/ip/address.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>

namespace kiko {
namespace {

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  throw KikoError("invalid hex character");
}

std::uint16_t checked_endpoint_port(std::uint64_t port, const std::string& value, bool allow_zero) {
  const std::uint64_t min_port = allow_zero ? 0 : 1;
  if (port < min_port || port > 65535) throw KikoError("endpoint port out of range: " + value);
  return static_cast<std::uint16_t>(port);
}

std::uint64_t parse_port_number(const std::string& text, const std::string& value) {
  auto port = parse_u64_strict(text);
  if (!port) throw KikoError("endpoint port is invalid: " + value);
  return *port;
}

Endpoint parse_endpoint_impl(const std::string& value, std::uint16_t default_port, bool allow_zero_port,
                             bool require_port_when_default_zero) {
  // Bracketed IPv6 form: [host]:port or [host].
  if (!value.empty() && value.front() == '[') {
    auto close = value.find(']');
    if (close == std::string::npos) throw KikoError("endpoint missing closing ']': " + value);
    auto host = value.substr(1, close - 1);
    auto rest = value.substr(close + 1);
    if (rest.empty()) {
      if (default_port == 0 && require_port_when_default_zero) throw KikoError("endpoint must be [host]:port: " + value);
      return Endpoint{host, default_port};
    }
    if (rest.front() != ':' || rest.size() < 2) throw KikoError("endpoint malformed after ']': " + value);
    auto port = parse_port_number(rest.substr(1), value);
    return Endpoint{host, checked_endpoint_port(port, value, allow_zero_port)};
  }

  // Bare IPv6 (more than one colon, no brackets): treat whole value as host.
  if (std::count(value.begin(), value.end(), ':') > 1) {
    if (default_port == 0 && require_port_when_default_zero) {
      throw KikoError("endpoint must be [host]:port for IPv6: " + value);
    }
    return Endpoint{value, default_port};
  }

  auto colon = value.rfind(':');
  if (colon == std::string::npos) {
    if (default_port == 0 && require_port_when_default_zero) {
      throw KikoError("endpoint must be host:port: " + value);
    }
    return Endpoint{value, default_port};
  }

  auto host = value.substr(0, colon);
  auto port_text = value.substr(colon + 1);
  if (host.empty()) host = "::";
  if (port_text.empty()) throw KikoError("endpoint port is empty: " + value);

  auto port = parse_port_number(port_text, value);
  return Endpoint{host, checked_endpoint_port(port, value, allow_zero_port)};
}

std::string normalize_ip_host(std::string_view host) {
  if (host.size() >= 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
  const auto zone = host.find('%');
  if (zone != std::string_view::npos) host = host.substr(0, zone);
  return std::string(host);
}

std::optional<asio::ip::address> parse_ip_address(std::string_view host) {
  asio::error_code ec;
  auto address = asio::ip::make_address(normalize_ip_host(host), ec);
  if (ec) return std::nullopt;
  return address;
}

bool ipv4_private(std::uint32_t value) {
  const auto a = static_cast<std::uint8_t>((value >> 24) & 0xff);
  const auto b = static_cast<std::uint8_t>((value >> 16) & 0xff);
  return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168);
}

bool ipv4_link_local(std::uint32_t value) {
  const auto a = static_cast<std::uint8_t>((value >> 24) & 0xff);
  const auto b = static_cast<std::uint8_t>((value >> 16) & 0xff);
  return a == 169 && b == 254;
}

}  // namespace

Endpoint parse_endpoint(const std::string& value, std::uint16_t default_port) {
  return parse_endpoint_impl(value, default_port, false, true);
}

Endpoint parse_bind_endpoint(const std::string& value, std::uint16_t default_port) {
  return parse_endpoint_impl(value, default_port, true, false);
}

IpAddressFamily ip_address_family(std::string_view host) {
  const auto address = parse_ip_address(host);
  if (!address) return IpAddressFamily::Unknown;
  return address->is_v6() ? IpAddressFamily::IPv6 : IpAddressFamily::IPv4;
}

IpAddressScope ip_address_scope(std::string_view host) {
  const auto address = parse_ip_address(host);
  if (!address) return IpAddressScope::Unknown;
  if (address->is_unspecified()) return IpAddressScope::Unknown;
  if (address->is_loopback()) return IpAddressScope::Loopback;

  if (address->is_v4()) {
    const auto value = address->to_v4().to_uint();
    if (ipv4_link_local(value)) return IpAddressScope::LinkLocal;
    if (ipv4_private(value)) return IpAddressScope::Private;
    if (address->is_multicast()) return IpAddressScope::Unknown;
    return IpAddressScope::Global;
  }

  const auto bytes = address->to_v6().to_bytes();
  if (bytes[0] == 0xfe && (bytes[1] & 0xc0) == 0x80) return IpAddressScope::LinkLocal;
  if ((bytes[0] & 0xfe) == 0xfc) return IpAddressScope::UniqueLocal;
  if (address->is_multicast()) return IpAddressScope::Unknown;
  return IpAddressScope::Global;
}

const char* ip_address_family_name(IpAddressFamily family) {
  switch (family) {
    case IpAddressFamily::IPv4:
      return "ipv4";
    case IpAddressFamily::IPv6:
      return "ipv6";
    case IpAddressFamily::Unknown:
      return "unknown";
  }
  return "unknown";
}

const char* ip_address_scope_name(IpAddressScope scope) {
  switch (scope) {
    case IpAddressScope::Loopback:
      return "loopback";
    case IpAddressScope::LinkLocal:
      return "link_local";
    case IpAddressScope::Private:
      return "private";
    case IpAddressScope::UniqueLocal:
      return "unique_local";
    case IpAddressScope::Global:
      return "global";
    case IpAddressScope::Unknown:
      return "unknown";
  }
  return "unknown";
}

bool is_ipv6_address(std::string_view host) { return ip_address_family(host) == IpAddressFamily::IPv6; }

bool is_global_ipv6_address(std::string_view host) {
  return ip_address_family(host) == IpAddressFamily::IPv6 && ip_address_scope(host) == IpAddressScope::Global;
}

std::size_t count_global_ipv6_addresses(const std::vector<std::string>& hosts) {
  std::size_t count = 0;
  std::vector<std::string> seen;
  for (const auto& host : hosts) {
    if (!is_global_ipv6_address(host)) continue;
    if (std::find(seen.begin(), seen.end(), host) != seen.end()) continue;
    seen.push_back(host);
    ++count;
  }
  return count;
}

std::string random_code(std::size_t bytes) {
  static constexpr char alphabet[] = "23456789abcdefghijkmnpqrstuvwxyz";
  std::random_device rd;
  std::mt19937_64 rng(rd());
  std::uniform_int_distribution<std::size_t> dist(0, sizeof(alphabet) - 2);

  std::string out;
  out.reserve(bytes * 2);
  for (std::size_t i = 0; i < bytes * 2; ++i) {
    out.push_back(alphabet[dist(rng)]);
  }
  return out;
}

std::string random_mnemonic_code(std::size_t words) {
  std::random_device rd;
  std::mt19937_64 rng(rd());
  std::uniform_int_distribution<int> room_dist(1000, 9999);
  std::uniform_int_distribution<std::size_t> word_dist(0, kWordList.size() - 1);

  std::string out = std::to_string(room_dist(rng));
  if (words == 0) words = 1;
  for (std::size_t i = 0; i < words; ++i) {
    out.push_back('-');
    out.append(kWordList[word_dist(rng)]);
  }
  return out;
}

namespace {

bool short_pairing_char(char c) {
  static constexpr char alphabet[] = "23456789abcdefghijkmnpqrstuvwxyz";
  return std::strchr(alphabet, c) != nullptr;
}

bool mnemonic_pairing_char(char c) {
  return short_pairing_char(c) || c == '-' || (c >= 'A' && c <= 'Z');
}

}  // namespace

std::optional<std::string> validate_pairing_code_format(const std::string& code, bool required) {
  if (code.empty()) {
    return required ? std::optional<std::string>("pairing code is required") : std::nullopt;
  }
  if (code.size() > 200) return "pairing code is too long (max 200)";
  for (const char c : code) {
    if (c == '\n' || c == '\r' || c == '\t') return "pairing code contains invalid whitespace";
  }

  const bool mnemonic = code.find('-') != std::string::npos;
  for (const char c : code) {
    if (mnemonic) {
      if (!mnemonic_pairing_char(c)) {
        return "pairing code has invalid character (use a-z, 0-9, and - for mnemonic codes)";
      }
    } else if (!short_pairing_char(c)) {
      return "pairing code has invalid character (use 23456789abcdefghijkmnpqrstuvwxyz)";
    }
  }

  if (mnemonic) {
    if (code.front() == '-' || code.back() == '-') return "pairing code has empty mnemonic segment";
    std::size_t start = 0;
    while (start < code.size()) {
      const auto end = code.find('-', start);
      const auto len = (end == std::string::npos ? code.size() : end) - start;
      if (len == 0) return "pairing code has empty mnemonic segment";
      start = end == std::string::npos ? code.size() : end + 1;
    }
  } else if (code.size() < 2) {
    return "pairing code is too short";
  }

  return std::nullopt;
}

std::optional<std::uint64_t> parse_u64_strict(const std::string& value) {
  if (value.empty()) return std::nullopt;
  std::uint64_t out = 0;
  for (unsigned char c : value) {
    if (!std::isdigit(c)) return std::nullopt;
    const auto digit = static_cast<std::uint64_t>(c - '0');
    if (out > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return std::nullopt;
    out = out * 10 + digit;
  }
  return out;
}

std::vector<std::string> split_csv(const std::string& value) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= value.size()) {
    auto comma = value.find(',', start);
    auto token = value.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    if (!token.empty()) out.push_back(token);
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return out;
}

std::string join_csv(const std::vector<std::string>& values) {
  std::string out;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i) out.push_back(',');
    out += values[i];
  }
  return out;
}

std::string hex_encode(const Bytes& bytes) {
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (auto b : bytes) oss << std::setw(2) << static_cast<int>(b);
  return oss.str();
}

Bytes hex_decode(const std::string& hex) {
  if (hex.size() % 2 != 0) throw KikoError("hex string has odd length");
  Bytes out;
  out.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    out.push_back(static_cast<std::uint8_t>((hex_value(hex[i]) << 4) | hex_value(hex[i + 1])));
  }
  return out;
}

std::string trim(const std::string& value) {
  auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); });
  auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) { return std::isspace(c); }).base();
  if (begin >= end) return {};
  return std::string(begin, end);
}

std::uint64_t now_ms() {
  auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

}  // namespace kiko
