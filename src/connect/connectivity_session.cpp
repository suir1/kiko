#include "connectivity_session.hpp"

#include "core/common.hpp"
#include "direct_session.hpp"

#include <atomic>
#include <utility>

namespace kiko {

RelayPeerResult wait_for_connectivity_peer(const ConnectivityRendezvous& rendezvous) {
  auto peer_result = race_until_peer(rendezvous.entries, rendezvous.hello, rendezvous.deadline,
                                     rendezvous.connect_options, rendezvous.relay_pass, rendezvous.cancel);
  if (!peer_result) throw KikoError(rendezvous.failure_message);
  return std::move(*peer_result);
}

RouteSelection select_connectivity_route(TcpSocket relay, const ConnectivitySession& session, AdaptivePuncher& puncher,
                                         ProgressReporter& reporter) {
  auto direct_attempt = [&](const std::atomic_bool* cancel) {
    return attempt_direct(session.role, session.listener, session.peer, session.direct_extra_candidates, puncher,
                          session.self_nat, session.peer_nat, session.route_plan, session.room,
                          session.connect_options, &reporter, cancel);
  };

  if (session.peer.route_commit_v2) {
    return race_transfer_route(std::move(relay), direct_attempt, puncher, session.route_plan, reporter,
                               session.confirmation_timeout, session.timing, session.cancel);
  }

  return select_transfer_route(std::move(relay), direct_attempt(session.cancel), puncher, session.route_plan, reporter,
                               session.confirmation_timeout, session.timing, session.cancel);
}

}  // namespace kiko
