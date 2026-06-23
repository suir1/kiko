#pragma once

#include "connectivity.hpp"
#include "core/progress.hpp"
#include "core/protocol.hpp"

#include <optional>
#include <vector>

namespace kiko {

struct FileEntry;

[[nodiscard]] RoutePlan build_route_plan(bool no_direct, ConnectivitySnapshot snapshot,
                                         const std::optional<StunProbeResult>& stun, int connections);

void apply_peer_direct_policy(RoutePlan& plan, const Message& peer);

void fill_transfer_snapshot(ConnectivitySnapshot& snapshot, const std::vector<FileEntry>& files, int connections_hint);

[[nodiscard]] RoutePlan resolve_route_plan(bool no_direct, ConnectivitySnapshot snapshot,
                                           const std::optional<StunProbeResult>& stun, int connections,
                                           bool ai_route, bool ai_route_plan_only,
                                           bool ai_route_connectivity_only, ProgressReporter& reporter);

[[nodiscard]] RoutePlan merge_route_plan_hint(const RoutePlan& rules, const RoutePlan& hint);

void explain_direct_failure(const ConnectivitySnapshot& snapshot, const RoutePlan& plan, bool ai_route,
                            ProgressReporter& reporter);

}  // namespace kiko
