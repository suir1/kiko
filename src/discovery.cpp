#include "discovery.hpp"

#include "io.hpp"
#include "platform.hpp"

#include <asio/ip/multicast.hpp>
#include <asio/ip/udp.hpp>

#include <algorithm>
#include <array>
#include <cstring>

namespace kiko {
namespace {

std::string discovery_payload(std::uint16_t port) { return std::string(kDiscoveryMagic) + std::to_string(port); }

std::optional<std::uint16_t> parse_discovery_payload(const std::string& payload) {
  if (payload.size() <= std::strlen(kDiscoveryMagic)) return std::nullopt;
  if (payload.compare(0, std::strlen(kDiscoveryMagic), kDiscoveryMagic) != 0) return std::nullopt;
  auto port = parse_u64_strict(payload.substr(std::strlen(kDiscoveryMagic)));
  if (!port || *port == 0 || *port > 65535) return std::nullopt;
  return static_cast<std::uint16_t>(*port);
}

void send_multicast(asio::ip::udp::socket& socket, const asio::ip::udp::endpoint& target, const std::string& payload) {
  asio::error_code ec;
  socket.send_to(asio::buffer(payload), target, 0, ec);
}

void setup_v4_socket(asio::ip::udp::socket& socket) {
  asio::error_code ec;
  socket.open(asio::ip::udp::v4(), ec);
  if (ec) return;
  socket.set_option(asio::socket_base::reuse_address(true), ec);
  socket.set_option(asio::ip::multicast::enable_loopback(true), ec);
}

void setup_v6_socket(asio::ip::udp::socket& socket) {
  asio::error_code ec;
  socket.open(asio::ip::udp::v6(), ec);
  if (ec) return;
  socket.set_option(asio::socket_base::reuse_address(true), ec);
  socket.set_option(asio::ip::multicast::enable_loopback(true), ec);
}

}  // namespace

void lan_announce(std::uint16_t port, std::atomic<bool>& stop) {
  const auto payload = discovery_payload(port);
  asio::ip::udp::socket v4(io_context());
  asio::ip::udp::socket v6(io_context());
  setup_v4_socket(v4);
  setup_v6_socket(v6);

  asio::error_code ec;
  auto v4_target = asio::ip::udp::endpoint(asio::ip::make_address(kMulticastV4, ec), kDiscoveryPort);
  auto v6_target = asio::ip::udp::endpoint(asio::ip::make_address(kMulticastV6, ec), kDiscoveryPort);

  while (!stop.load()) {
    if (v4.is_open()) send_multicast(v4, v4_target, payload);
    if (v6.is_open()) send_multicast(v6, v6_target, payload);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

std::vector<Endpoint> lan_discover(std::chrono::milliseconds timeout) {
  std::vector<Endpoint> out;
  asio::ip::udp::socket v4(io_context());
  asio::ip::udp::socket v6(io_context());
  setup_v4_socket(v4);
  setup_v6_socket(v6);

  asio::error_code ec;
  if (v4.is_open()) {
    v4.bind(asio::ip::udp::endpoint(asio::ip::udp::v4(), kDiscoveryPort), ec);
    if (!ec) {
      v4.set_option(asio::ip::multicast::join_group(asio::ip::make_address(kMulticastV4, ec)), ec);
      v4.non_blocking(true, ec);
    }
  }
  if (v6.is_open()) {
    ec.clear();
    v6.bind(asio::ip::udp::endpoint(asio::ip::udp::v6(), kDiscoveryPort), ec);
    if (!ec) {
      v6.set_option(asio::ip::multicast::join_group(asio::ip::make_address(kMulticastV6, ec)), ec);
      v6.non_blocking(true, ec);
    }
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::array<char, 512> buf{};
  while (std::chrono::steady_clock::now() < deadline) {
    for (asio::ip::udp::socket* sock : {&v4, &v6}) {
      if (!sock->is_open()) continue;
      asio::ip::udp::endpoint from;
      ec.clear();
      auto n = sock->receive_from(asio::buffer(buf), from, 0, ec);
      if (ec == asio::error::would_block) continue;
      if (ec) continue;
      if (n <= 0) continue;
      auto port = parse_discovery_payload(std::string(buf.data(), n));
      if (!port) continue;
      Endpoint ep{from.address().to_string(), *port};
      if (std::find_if(out.begin(), out.end(), [&](const Endpoint& e) {
            return e.host == ep.host && e.port == ep.port;
          }) == out.end()) {
        out.push_back(ep);
      }
    }
    io_context().poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return out;
}

}  // namespace kiko
