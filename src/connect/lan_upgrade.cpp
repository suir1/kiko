#include "connect/lan_upgrade.hpp"

#include "core/common.hpp"
#include "platform/platform.hpp"
#include "core/protocol.hpp"

#include <chrono>

namespace kiko {
namespace {

constexpr auto kRelayUpgradeTimeout = std::chrono::seconds(2);

std::vector<Endpoint> local_candidates_from_message(const Message& message) {
  std::vector<Endpoint> out;
  auto port = message_port_field(message, "listen_port");
  if (!port) return out;
  auto push = [&](const std::string& host) {
    const Endpoint candidate{host, *port};
    if (candidate.is_unspecified()) return;
    for (const auto& existing : out) {
      if (existing == candidate) return;
    }
    out.push_back(candidate);
  };
  for (const auto& host : split_csv(message.get("local_candidates"))) {
    push(host);
  }
  if (!message.get("listen_host").empty()) {
    push(message.get("listen_host"));
  }
  return out;
}

}  // namespace

TcpSocket resolve_relay_channel(Role role, TcpSocket relay, TcpListener& listener, std::uint16_t listen_port,
                                const std::vector<std::string>& local_addrs, bool no_direct) {
  if (no_direct || !relay.valid()) return relay;

  if (role == Role::Receiver) {
    send_message(relay, Message{"ips_request", {}});
    auto response = recv_message_timeout(relay, kRelayUpgradeTimeout);
    if (response && response->type == "ips_response") {
      for (const auto& candidate : local_candidates_from_message(*response)) {
        auto direct = connect_tcp(candidate, std::chrono::milliseconds(500));
        if (direct.valid()) {
          relay.close();
          return direct;
        }
      }
    }
    return relay;
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    auto accepted = listener.accept(std::chrono::milliseconds(80));
    if (accepted.valid()) {
      relay.close();
      return accepted;
    }
    const int fd = relay.fd();
    if (fd >= 0 && net_poll(fd, true, false, 80) > 0) {
      if (auto msg = recv_message_timeout(relay, std::chrono::milliseconds(80))) {
        if (msg->type == "ips_request") {
          send_message(relay, Message{"ips_response",
                                      {{"listen_port", std::to_string(listen_port)},
                                       {"listen_host", listener.local_endpoint().host},
                                       {"local_candidates", join_csv(local_addrs)}}});
        }
      }
    }
  }
  return relay;
}

}  // namespace kiko
