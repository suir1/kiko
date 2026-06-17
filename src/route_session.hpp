#pragma once

#include "adaptive.hpp"
#include "connectivity.hpp"
#include "progress.hpp"
#include "socket.hpp"

#include <chrono>
#include <optional>

namespace kiko {

enum class RoutePath { Direct, Relay };

struct RouteSelection {
  RoutePath path = RoutePath::Relay;
  TcpSocket relay;
  std::optional<TcpSocket> direct;
  bool allow_lan_upgrade = false;
  PunchStats punch_stats;
  bool explain_direct_failure = false;
};

[[nodiscard]] RouteSelection select_transfer_route(TcpSocket relay, std::optional<TcpSocket> direct,
                                                   const AdaptivePuncher& puncher, const RoutePlan& route_plan,
                                                   ProgressReporter& reporter,
                                                   std::chrono::milliseconds confirmation_timeout);

}  // namespace kiko
