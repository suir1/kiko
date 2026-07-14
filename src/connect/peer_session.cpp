#include "connect/peer_session.hpp"

#include "connect/peer_route_session.hpp"
#include "connect/route_planner.hpp"

#include <map>
#include <utility>

namespace kiko {
namespace {

std::string describe_peer_route_plan(const RoutePlan& plan) {
  std::string line = plan.reason;
  if (plan.skip_direct) return line + " (skip direct)";
  line += " direct_window=" + std::to_string(plan.direct_timeout.count()) + "ms";
  line += " direct_connect=" + std::to_string(plan.direct_connect.count()) + "ms";
  if (plan.udp_punch_enabled) line += " udp-assist";
  return line;
}

}  // namespace

PeerSession open_peer_session(const PeerSessionConfig& config, ProgressReporter& reporter) {
  PeerRouteSessionConfig route_config;
  route_config.connection = config;
  route_config.role = config.role;
  route_config.code = config.code;
  route_config.show_qrcode = config.show_qrcode;
  route_config.app = config.app;
  route_config.run_stun_probe = config.udp_probe;

  PeerRouteSession route(std::move(route_config), reporter);
  std::map<std::string, std::string> hello_fields;
  if (config.role == Role::Sender) hello_fields["conn_count"] = "1";
  route.rendezvous(hello_fields);

  auto plan = build_route_plan(config.no_direct, route.connectivity_snapshot(0), route.stun(), 1);
  plan = route.apply_peer_policy(std::move(plan));
  reporter.status("route plan: " + describe_peer_route_plan(plan));

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
