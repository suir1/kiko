#include "route_planner.hpp"

#include "ai_advisor.hpp"
#include "ai_client.hpp"
#include "profile.hpp"
#include "transfer.hpp"
#include "transfer_heuristics.hpp"

#include <algorithm>

namespace kiko {
namespace {

bool route_reason_allows_profile_shortening(const RoutePlan& plan) {
  return !plan.skip_direct && (plan.reason == "default" || plan.reason == "stun_cone_direct_probe" ||
                               plan.reason == "ipv6_global_direct");
}

int failure_count_for_kind(const NetworkProfileEntry& profile, const std::string& kind) {
  const auto it = profile.candidate_failures_by_kind.find(kind);
  return it == profile.candidate_failures_by_kind.end() ? 0 : it->second;
}

std::string normalized_direct_kind(std::string kind) {
  if (kind == "public-same-port") return "public";
  if (kind == "manual" || kind == "discovered" || kind == "lan" || kind == "listen" ||
      kind == "ipv6_global" || kind == "public") {
    return kind;
  }
  return {};
}

void push_unique_kind(std::vector<std::string>& kinds, const std::string& kind) {
  if (kind.empty()) return;
  if (std::find(kinds.begin(), kinds.end(), kind) == kinds.end()) kinds.push_back(kind);
}

void prefer_prior_direct_kind(RoutePlan& plan, const NetworkProfileEntry& profile) {
  std::vector<std::string> order;
  push_unique_kind(order, normalized_direct_kind(profile.last_direct_candidate_kind));
  for (const auto& kind : plan.direct_candidate_order) push_unique_kind(order, kind);
  plan.direct_candidate_order = std::move(order);
}

void prefer_local_before_public(RoutePlan& plan) {
  std::vector<std::string> order;
  push_unique_kind(order, "lan");
  push_unique_kind(order, "listen");
  push_unique_kind(order, "manual");
  push_unique_kind(order, "discovered");
  push_unique_kind(order, "ipv6_global");
  push_unique_kind(order, "public");
  for (const auto& kind : plan.direct_candidate_order) push_unique_kind(order, kind);
  plan.direct_candidate_order = std::move(order);
}

void apply_same_port_history(RoutePlan& plan, const NetworkProfileEntry& profile) {
  if (plan.skip_direct) return;
  if (profile.same_port_successes > 0 && profile.same_port_failure_streak == 0) {
    plan.same_port_timeout = std::max(plan.same_port_timeout, std::chrono::milliseconds(650));
    plan.same_port_connect = std::max(plan.same_port_connect, std::chrono::milliseconds(180));
    return;
  }

  if (profile.same_port_failure_streak >= 4) {
    plan.same_port_timeout = std::chrono::milliseconds(180);
    plan.same_port_connect = std::chrono::milliseconds(100);
  }
}

void apply_profile_route_history(RoutePlan& plan, const NetworkProfileEntry& profile) {
  apply_same_port_history(plan, profile);
  if (!profile.last_direct_candidate_kind.empty()) prefer_prior_direct_kind(plan, profile);

  if (profile.last_path == "direct") return;

  const int ipv6_failures = failure_count_for_kind(profile, "ipv6_global");
  if (ipv6_failures >= 2 && route_reason_allows_profile_shortening(plan)) {
    if (plan.direct_timeout > std::chrono::milliseconds(900)) {
      plan.direct_timeout = std::chrono::milliseconds(900);
      plan.direct_connect = std::chrono::milliseconds(250);
    }
    plan.reason = "profile_ipv6_failures_short_direct";
    prefer_local_before_public(plan);
  }

  const int public_failures = failure_count_for_kind(profile, "public") + failure_count_for_kind(profile, "public-same-port");
  if (public_failures >= 2 && route_reason_allows_profile_shortening(plan)) {
    if (plan.direct_timeout > std::chrono::milliseconds(900)) {
      plan.direct_timeout = std::chrono::milliseconds(900);
      plan.direct_connect = std::chrono::milliseconds(250);
    }
    plan.reason = "profile_public_failures_short_direct";
    prefer_local_before_public(plan);
  }

  if (profile.last_path == "relay" && profile.path_streak >= 2 && !plan.skip_direct &&
      (plan.reason == "default" || plan.reason == "profile_public_failures_short_direct")) {
    plan.direct_timeout = std::chrono::milliseconds(600);
    plan.direct_connect = std::chrono::milliseconds(220);
    plan.reason = "profile_relay_history_short_direct";
  }
}

}  // namespace

RoutePlan build_route_plan(bool no_direct, ConnectivitySnapshot snapshot, const std::optional<StunProbeResult>& stun,
                           int connections) {
  RuleScheduler scheduler;
  const auto profile = load_profile(network_fingerprint());
  if (profile) apply_profile_to_snapshot(*profile, snapshot);
  auto plan = scheduler.plan(snapshot, stun, no_direct, connections);
  if (profile) apply_profile_route_history(plan, *profile);
  return plan;
}

void apply_peer_direct_policy(RoutePlan& plan, const Message& peer) {
  if (peer.get("peer_no_direct") != "1") return;
  plan.skip_direct = true;
  plan.direct_timeout = std::chrono::milliseconds(0);
  plan.direct_connect = std::chrono::milliseconds(0);
  plan.udp_punch_enabled = false;
  plan.reason = "peer_no_direct";
}

void fill_transfer_snapshot(ConnectivitySnapshot& snapshot, const std::vector<FileEntry>& files, int connections_hint) {
  const auto stats = transfer_payload_stats(files);
  snapshot.file_count = files.size();
  snapshot.largest_file_bytes = stats.largest_file_bytes;
  snapshot.compressible_ratio = stats.compressible_ratio;
  snapshot.connections_hint = connections_hint;
}

RoutePlan resolve_route_plan(bool no_direct, ConnectivitySnapshot snapshot, const std::optional<StunProbeResult>& stun,
                             int connections, bool ai_route, bool ai_route_plan_only,
                             bool ai_route_connectivity_only, ProgressReporter& reporter) {
  auto plan = build_route_plan(no_direct, snapshot, stun, connections);
  if (!ai_route && !ai_route_plan_only) return plan;

  auto cfg = ai_config_from_env();
  if (!ai_configured(cfg)) return plan;

  cfg.timeout = std::chrono::milliseconds(400);
  const auto ai = ai_suggest_route_plan(snapshot, cfg);
  if (!ai.ok || !ai.plan) {
    if (ai_route || ai_route_plan_only) reporter.status("ai route: unavailable (" + ai.error + "), using rules");
    return plan;
  }

  const auto& ai_plan = *ai.plan;
  reporter.status("ai route plan: skip_direct=" + std::string(ai_plan.skip_direct ? "true" : "false") +
                  " udp_punch_enabled=" + std::string(ai_plan.udp_punch_enabled ? "true" : "false") +
                  " direct_timeout_ms=" + std::to_string(ai_plan.direct_timeout.count()) +
                  " connections=" + std::to_string(ai_plan.connections) + " reason=" + ai_plan.reason);
  if (!ai_plan.direct_candidate_order.empty()) {
    reporter.status("ai direct candidate order: " + join_csv(ai_plan.direct_candidate_order));
  }
  if (ai_route_plan_only) return plan;
  const nlohmann::json* raw = ai.parsed_plan ? &*ai.parsed_plan : nullptr;
  auto merged = merge_ai_route_plan(plan, ai_plan, raw);
  if (ai_route_connectivity_only) merged.connections = plan.connections;
  return merged;
}

RoutePlan merge_route_plan_hint(const RoutePlan& rules, const RoutePlan& hint) {
  return merge_ai_route_plan(rules, hint);
}

void explain_direct_failure(const ConnectivitySnapshot& snapshot, const RoutePlan& plan, bool ai_route,
                            ProgressReporter& reporter) {
  const auto hints = rule_direct_failure_hints(snapshot, plan);
  if (!hints.empty()) reporter.status(hints);

  if (!ai_route) return;
  auto cfg = ai_config_from_env();
  if (!ai_configured(cfg)) return;
  cfg.timeout = std::chrono::milliseconds(400);
  const auto ai = ai_explain_diagnosis(snapshot, plan, "direct connection failed after punch attempts", cfg);
  if (ai.ok && !ai.text.empty()) reporter.status("ai post-direct: " + ai.text);
}

}  // namespace kiko
