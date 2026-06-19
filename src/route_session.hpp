#pragma once

#include "adaptive.hpp"
#include "connectivity.hpp"
#include "progress.hpp"
#include "socket.hpp"

#include <atomic>
#include <chrono>
#include <functional>
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
  RouteOutcome outcome;
  RouteTiming timing;
};

[[nodiscard]] RouteSelection select_transfer_route(TcpSocket relay, std::optional<TcpSocket> direct,
                                                   const AdaptivePuncher& puncher, const RoutePlan& route_plan,
                                                   ProgressReporter& reporter,
                                                   std::chrono::milliseconds confirmation_timeout,
                                                   RouteTiming timing = {});

using DirectAttemptFn = std::function<std::optional<TcpSocket>(const std::atomic_bool*)>;

[[nodiscard]] RouteSelection race_transfer_route(TcpSocket relay, DirectAttemptFn direct_attempt,
                                                 const AdaptivePuncher& puncher, const RoutePlan& route_plan,
                                                 ProgressReporter& reporter,
                                                 std::chrono::milliseconds confirmation_timeout,
                                                 RouteTiming timing = {});

}  // namespace kiko
