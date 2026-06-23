#pragma once

#include "core/protocol.hpp"

#include <string>

namespace kiko {

enum class RelayRouteChoice { Waiting, Standby, DirectOk, RelayReady, Invalid };

struct RelayRouteDecision {
  enum class Kind { Pending, Direct, Relay, Invalid };
  Kind kind = Kind::Pending;
  std::string relay_reason;
};

[[nodiscard]] bool relay_route_has_presence(RelayRouteChoice choice);
[[nodiscard]] bool relay_route_choice_final(RelayRouteChoice choice);
[[nodiscard]] RelayRouteChoice relay_route_choice_from(const Message& msg);
void merge_relay_route_choice(RelayRouteChoice& state, RelayRouteChoice next);
[[nodiscard]] RelayRouteDecision relay_route_decision_for(RelayRouteChoice first, RelayRouteChoice second,
                                                          bool deadline_expired);

}  // namespace kiko
