#pragma once

#include "adaptive.hpp"
#include "connectivity.hpp"
#include "progress.hpp"
#include "protocol.hpp"
#include "relay/relay_race.hpp"
#include "route_session.hpp"
#include "socket.hpp"

#include <chrono>
#include <atomic>
#include <optional>
#include <string>
#include <vector>

namespace kiko {

struct ConnectivityRendezvous {
  std::vector<RelayRaceEntry> entries;
  Message hello;
  ConnectOptions connect_options;
  std::optional<std::string> relay_pass;
  std::chrono::milliseconds deadline = std::chrono::seconds(30);
  std::string failure_message = "failed to connect relay or rendezvous peer";
  const std::atomic_bool* cancel = nullptr;
};

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
  RouteTiming timing;
  const std::atomic_bool* cancel = nullptr;
};

[[nodiscard]] RelayPeerResult wait_for_connectivity_peer(const ConnectivityRendezvous& rendezvous);

[[nodiscard]] RouteSelection select_connectivity_route(TcpSocket relay, const ConnectivitySession& session,
                                                       AdaptivePuncher& puncher, ProgressReporter& reporter);

}  // namespace kiko
