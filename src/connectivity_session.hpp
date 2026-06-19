#pragma once

#include "adaptive.hpp"
#include "connectivity.hpp"
#include "progress.hpp"
#include "protocol.hpp"
#include "route_session.hpp"
#include "socket.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace kiko {

struct ConnectivitySession {
  Role role;
  TcpListener& listener;
  const Message& peer;
  std::vector<Endpoint> direct_extra_candidates;
  NatProfile self_nat;
  NatProfile peer_nat;
  RoutePlan route_plan;
  std::string room;
  ConnectOptions connect_options;
  std::chrono::milliseconds confirmation_timeout;
};

[[nodiscard]] RouteSelection select_connectivity_route(TcpSocket relay, const ConnectivitySession& session,
                                                       AdaptivePuncher& puncher, ProgressReporter& reporter);

}  // namespace kiko
