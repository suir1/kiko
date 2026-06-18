#pragma once

#include "adaptive.hpp"
#include "common.hpp"
#include "connectivity.hpp"
#include "protocol.hpp"
#include "proxy.hpp"
#include "socket.hpp"

#include <chrono>
#include <optional>
#include <vector>

namespace kiko {

class ProgressReporter;

struct DirectMuxResult {
  std::vector<TcpSocket> channels;
  bool mux_enabled = false;
  std::string fallback_reason;
};

[[nodiscard]] std::optional<TcpSocket> attempt_direct(Role role, TcpListener& listener, const Message& peer,
                                                      const std::vector<Endpoint>& lan_extra,
                                                      AdaptivePuncher& puncher, const NatProfile& self,
                                                      const NatProfile& peer_nat, const RoutePlan& route_plan,
                                                      const std::string& room,
                                                      const ConnectOptions& connect_options = ConnectOptions{},
                                                      ProgressReporter* reporter = nullptr);

// Opens auxiliary direct channels and confirms both peers are ready before
// switching to mux. If auxiliary setup fails on either peer, keeps channel 0
// usable for a single-connection direct transfer.
[[nodiscard]] DirectMuxResult negotiate_direct_mux_channels(
    TcpSocket primary, Role role, TcpListener& listener, const Message& peer, int connections,
    const std::string& room, const ConnectOptions& connect_options = ConnectOptions{},
    std::chrono::milliseconds setup_timeout = std::chrono::seconds(5));

}  // namespace kiko
