#include "relay_route_state.hpp"

#include <cassert>
#include <iostream>

int main() {
  using namespace kiko;

  {
    assert(relay_route_choice_from(Message{"relay_standby", {}}) == RelayRouteChoice::Standby);
    assert(relay_route_choice_from(Message{"direct_ok", {}}) == RelayRouteChoice::DirectOk);
    assert(relay_route_choice_from(Message{"relay_ready", {}}) == RelayRouteChoice::RelayReady);
    assert(relay_route_choice_from(Message{"unexpected", {}}) == RelayRouteChoice::Invalid);
  }

  {
    RelayRouteChoice state = RelayRouteChoice::Waiting;
    merge_relay_route_choice(state, RelayRouteChoice::Standby);
    assert(state == RelayRouteChoice::Standby);
    merge_relay_route_choice(state, RelayRouteChoice::DirectOk);
    assert(state == RelayRouteChoice::DirectOk);
    merge_relay_route_choice(state, RelayRouteChoice::RelayReady);
    assert(state == RelayRouteChoice::DirectOk);
  }

  {
    const auto decision =
        relay_route_decision_for(RelayRouteChoice::DirectOk, RelayRouteChoice::DirectOk, false);
    assert(decision.kind == RelayRouteDecision::Kind::Direct);
  }

  {
    const auto decision =
        relay_route_decision_for(RelayRouteChoice::RelayReady, RelayRouteChoice::RelayReady, false);
    assert(decision.kind == RelayRouteDecision::Kind::Relay);
    assert(decision.relay_reason == "relay");
  }

  {
    const auto decision =
        relay_route_decision_for(RelayRouteChoice::Standby, RelayRouteChoice::RelayReady, false);
    assert(decision.kind == RelayRouteDecision::Kind::Relay);
    assert(decision.relay_reason == "standby");
  }

  {
    const auto decision =
        relay_route_decision_for(RelayRouteChoice::DirectOk, RelayRouteChoice::RelayReady, false);
    assert(decision.kind == RelayRouteDecision::Kind::Relay);
    assert(decision.relay_reason == "mismatch");
  }

  {
    const auto pending =
        relay_route_decision_for(RelayRouteChoice::Standby, RelayRouteChoice::Standby, false);
    assert(pending.kind == RelayRouteDecision::Kind::Pending);
    const auto timeout =
        relay_route_decision_for(RelayRouteChoice::Standby, RelayRouteChoice::Standby, true);
    assert(timeout.kind == RelayRouteDecision::Kind::Relay);
    assert(timeout.relay_reason == "timeout");
  }

  {
    const auto decision =
        relay_route_decision_for(RelayRouteChoice::Invalid, RelayRouteChoice::Standby, false);
    assert(decision.kind == RelayRouteDecision::Kind::Invalid);
  }

  std::cout << "relay_route_state_test ok\n";
  return 0;
}
