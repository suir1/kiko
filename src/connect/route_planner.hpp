#pragma once

#include "connectivity.hpp"
#include "core/progress.hpp"

#include <optional>
#include <vector>

namespace kiko {

[[nodiscard]] RoutePlan build_route_plan(bool no_direct, const ConnectivitySnapshot& snapshot,
                                         const std::optional<StunProbeResult>& stun, int connections);

[[nodiscard]] std::string describe_route_plan(const RoutePlan& plan, bool include_same_port = false);

[[nodiscard]] PunchPlan build_direct_attempt_plan(const RoutePlan& plan, Role role, AdaptivePuncher& puncher,
                                                  const std::vector<DirectCandidate>& candidates,
                                                  const NatProfile& self, const NatProfile& peer);

void apply_direct_candidate_scoring(std::vector<DirectCandidate>& candidates,
                                    const RoutePlan::DirectCandidateScoreHints& hints,
                                    const std::vector<std::string>& kind_order = {});

void apply_peer_direct_policy(RoutePlan& plan, bool peer_no_direct);

[[nodiscard]] RoutePlan resolve_route_plan(bool no_direct, const ConnectivitySnapshot& snapshot,
                                           const std::optional<StunProbeResult>& stun, int connections,
                                           bool ai_route, bool ai_route_plan_only,
                                           bool ai_route_connectivity_only, ProgressReporter& reporter);

[[nodiscard]] RoutePlan merge_route_plan_hint(const RoutePlan& rules, const RoutePlan& hint);

void explain_direct_failure(const ConnectivitySnapshot& snapshot, const RoutePlan& plan, bool ai_route,
                            ProgressReporter& reporter);

}  // namespace kiko
