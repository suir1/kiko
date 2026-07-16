#include "relay_route_state.hpp"

namespace kiko {

static bool relay_route_has_presence(RelayRouteChoice choice) {
  return choice != RelayRouteChoice::Waiting && choice != RelayRouteChoice::Invalid;
}

static bool relay_route_choice_final(RelayRouteChoice choice) {
  return choice == RelayRouteChoice::DirectOk || choice == RelayRouteChoice::RelayReady ||
         choice == RelayRouteChoice::Invalid;
}

RelayRouteChoice relay_route_choice_from(const Message& msg) {
  if (msg.type == "relay_standby") return RelayRouteChoice::Standby;
  if (msg.type == "direct_ok") return RelayRouteChoice::DirectOk;
  if (msg.type == "relay_ready") return RelayRouteChoice::RelayReady;
  return RelayRouteChoice::Invalid;
}

void merge_relay_route_choice(RelayRouteChoice& state, RelayRouteChoice next) {
  if (state == RelayRouteChoice::Invalid || relay_route_choice_final(state)) return;
  if (next == RelayRouteChoice::Invalid || relay_route_choice_final(next) || state == RelayRouteChoice::Waiting) {
    state = next;
  }
}

RelayRouteDecision relay_route_decision_for(RelayRouteChoice first, RelayRouteChoice second, bool deadline_expired) {
  if (first == RelayRouteChoice::Invalid || second == RelayRouteChoice::Invalid) {
    return {RelayRouteDecision::Kind::Invalid, {}};
  }
  if (first == RelayRouteChoice::DirectOk && second == RelayRouteChoice::DirectOk) {
    return {RelayRouteDecision::Kind::Direct, {}};
  }

  const bool first_relay = first == RelayRouteChoice::RelayReady;
  const bool second_relay = second == RelayRouteChoice::RelayReady;
  if ((first_relay && relay_route_has_presence(second)) || (second_relay && relay_route_has_presence(first))) {
    if (first_relay && second_relay) return {RelayRouteDecision::Kind::Relay, "relay"};
    if (first == RelayRouteChoice::DirectOk || second == RelayRouteChoice::DirectOk) {
      return {RelayRouteDecision::Kind::Relay, "mismatch"};
    }
    return {RelayRouteDecision::Kind::Relay, "standby"};
  }

  if (deadline_expired && relay_route_has_presence(first) && relay_route_has_presence(second)) {
    return {RelayRouteDecision::Kind::Relay, "timeout"};
  }
  return {};
}

}  // namespace kiko
