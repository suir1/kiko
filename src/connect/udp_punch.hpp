#pragma once

#include "core/adaptive.hpp"
#include "core/common.hpp"
#include "connectivity.hpp"
#include "core/protocol.hpp"
#include "core/socket.hpp"

#include <atomic>

namespace kiko {

struct UdpPunchParams {
  Role role = Role::Sender;
  Endpoint peer_wan{};
  std::string token;
  std::chrono::milliseconds window{400};
  const std::atomic_bool* cancel = nullptr;
};

// Sends synchronized UDP probes to peer's WAN listen endpoint (NAT mapping assist only).
void udp_punch_burst(const UdpPunchParams& params);

// UDP poke window + existing TCP punch plan. Returns connected socket if TCP path wins.
[[nodiscard]] std::optional<TcpSocket> try_udp_assisted_direct(Role role, TcpListener& listener,
                                                               const Endpoint& peer_wan, const std::string& punch_token,
                                                               PunchPlan plan, AdaptivePuncher& puncher,
                                                               const std::string& room,
                                                               const ConnectOptions& connect_options = ConnectOptions{},
                                                               const std::atomic_bool* cancel = nullptr);

}  // namespace kiko
