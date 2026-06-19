#include "connectivity_session.hpp"

#include "direct_session.hpp"

#include <atomic>
#include <utility>

namespace kiko {

RouteSelection select_connectivity_route(TcpSocket relay, const ConnectivitySession& session, AdaptivePuncher& puncher,
                                         ProgressReporter& reporter) {
  auto direct_attempt = [&](const std::atomic_bool* cancel) {
    return attempt_direct(session.role, session.listener, session.peer, session.direct_extra_candidates, puncher,
                          session.self_nat, session.peer_nat, session.route_plan, session.room,
                          session.connect_options, &reporter, cancel);
  };

  if (session.peer.get("route_commit") == "v2") {
    return race_transfer_route(std::move(relay), direct_attempt, puncher, session.route_plan, reporter,
                               session.confirmation_timeout);
  }

  return select_transfer_route(std::move(relay), direct_attempt(nullptr), puncher, session.route_plan, reporter,
                               session.confirmation_timeout);
}

}  // namespace kiko
