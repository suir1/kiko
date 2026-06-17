#include "route_planner.hpp"

#include "ai_advisor.hpp"
#include "ai_client.hpp"
#include "profile.hpp"
#include "transfer.hpp"
#include "transfer_heuristics.hpp"

namespace kiko {

RoutePlan build_route_plan(bool no_direct, ConnectivitySnapshot snapshot, const std::optional<StunProbeResult>& stun,
                           int connections) {
  RuleScheduler scheduler;
  if (auto profile = load_profile(network_fingerprint())) apply_profile_to_snapshot(*profile, snapshot);
  auto plan = scheduler.plan(snapshot, stun, no_direct, connections);
  if (auto prior = load_profile(network_fingerprint())) {
    if (prior->last_path == "relay" && prior->success_count >= 2 && !plan.skip_direct && plan.reason == "default") {
      plan.direct_timeout = std::chrono::milliseconds(600);
      plan.direct_connect = std::chrono::milliseconds(220);
      plan.reason = "profile_relay_history_short_direct";
    }
  }
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
