#include "connect/peer_session.hpp"

#include "connect/peer_route_session.hpp"
#include "connect/route_planner.hpp"

#include <utility>

namespace kiko {

PeerSession open_peer_session(const PeerSessionConfig& config, ProgressReporter& reporter) {
  PeerRouteSessionConfig route_config;
  route_config.connection = config;
  route_config.role = config.role;
  route_config.code = config.code;
  route_config.show_qrcode = config.show_qrcode;
  route_config.app = config.app;
  route_config.run_stun_probe = config.udp_probe;

  PeerRouteSession route(std::move(route_config), reporter);
  route.rendezvous();

  auto plan = build_route_plan(config.no_direct, route.connectivity_snapshot(0), route.stun_probe(), 1);

  auto established = route.establish(std::move(plan));
  if (established.channels.empty()) throw KikoError("peer route did not establish a channel");
  auto code = route.code();
  return PeerSession{EncryptedSession{std::move(established.channels.front()), std::move(established.key),
                                      established.timing},
                     std::move(code),
                     std::move(established.outcome),
                     std::move(established.active_relay),
                     std::move(established.relay_keepalive)};
}

}  // namespace kiko
