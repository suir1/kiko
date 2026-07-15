#include "route_planner.hpp"

#include "diagnostics/ai_advisor.hpp"
#include "diagnostics/ai_client.hpp"

#include <algorithm>
#include <initializer_list>

namespace kiko {
namespace {

constexpr auto kRelayStandbyDirectWindow = std::chrono::milliseconds(500);
constexpr auto kRelayStandbyConnectWindow = std::chrono::milliseconds(220);

bool route_reason_allows_profile_shortening(const RoutePlan& plan) {
  return !plan.skip_direct && (plan.reason == "default" || plan.reason == "stun_cone_direct_probe" ||
                               plan.reason == "ipv6_global_direct");
}

int failure_count_for_kind(const ConnectivitySnapshot& snapshot, const std::string& kind) {
  const auto it = snapshot.profile.candidate_failures_by_kind.find(kind);
  return it == snapshot.profile.candidate_failures_by_kind.end() ? 0 : it->second;
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

void prepend_candidate_order(RoutePlan& plan, std::initializer_list<std::string> preferred) {
  std::vector<std::string> order;
  order.reserve(preferred.size() + plan.direct_candidate_order.size());
  for (const auto& kind : preferred) push_unique_kind(order, kind);
  for (const auto& kind : plan.direct_candidate_order) push_unique_kind(order, kind);
  plan.direct_candidate_order = std::move(order);
}

void prefer_prior_direct_kind(RoutePlan& plan, const ConnectivitySnapshot& snapshot) {
  prepend_candidate_order(plan, {normalized_direct_kind(snapshot.profile.last_direct_candidate_kind)});
}

void prefer_local_before_public(RoutePlan& plan) {
  prepend_candidate_order(plan, {"lan", "listen", "manual", "discovered", "ipv6_global", "public"});
}

void prefer_global_ipv6(RoutePlan& plan) {
  prepend_candidate_order(plan, {"manual", "discovered", "lan", "listen", "ipv6_global", "public"});
}

void fill_candidate_score_hints(RoutePlan& plan, const ConnectivitySnapshot& snapshot) {
  plan.candidate_score_hints.vpn_detected = snapshot.vpn_detected;
  plan.candidate_score_hints.profile_last_path = snapshot.profile.last_path;
  plan.candidate_score_hints.profile_direct_candidate_kind = snapshot.profile.last_direct_candidate_kind;
  plan.candidate_score_hints.profile_candidate_failures_by_kind = snapshot.profile.candidate_failures_by_kind;
}

std::optional<AiHttpConfig> route_ai_config() {
  auto config = ai_config_from_env();
  if (!ai_configured(config)) return std::nullopt;
  config.timeout = std::chrono::milliseconds(400);
  return config;
}

void apply_same_port_history(RoutePlan& plan, const ConnectivitySnapshot& snapshot) {
  if (plan.skip_direct) return;
  if (snapshot.profile.same_port_successes > 0 && snapshot.profile.same_port_failure_streak == 0) {
    plan.same_port_timeout = std::max(plan.same_port_timeout, std::chrono::milliseconds(650));
    plan.same_port_connect = std::max(plan.same_port_connect, std::chrono::milliseconds(180));
    return;
  }

  if (snapshot.profile.same_port_failure_streak >= 4) {
    plan.same_port_timeout = std::chrono::milliseconds(180);
    plan.same_port_connect = std::chrono::milliseconds(100);
  }
}

void apply_profile_route_history(RoutePlan& plan, const ConnectivitySnapshot& snapshot) {
  apply_same_port_history(plan, snapshot);
  if (!snapshot.profile.last_direct_candidate_kind.empty()) prefer_prior_direct_kind(plan, snapshot);

  if (snapshot.profile.last_path == "direct") return;

  const int ipv6_failures = failure_count_for_kind(snapshot, "ipv6_global");
  if (ipv6_failures >= 2 && route_reason_allows_profile_shortening(plan)) {
    if (plan.direct_timeout > std::chrono::milliseconds(900)) {
      plan.direct_timeout = std::chrono::milliseconds(900);
      plan.direct_connect = std::chrono::milliseconds(250);
    }
    plan.reason = "profile_ipv6_failures_short_direct";
    prefer_local_before_public(plan);
  }

  const int public_failures =
      failure_count_for_kind(snapshot, "public") + failure_count_for_kind(snapshot, "public-same-port");
  if (public_failures >= 2 && route_reason_allows_profile_shortening(plan)) {
    if (plan.direct_timeout > std::chrono::milliseconds(900)) {
      plan.direct_timeout = std::chrono::milliseconds(900);
      plan.direct_connect = std::chrono::milliseconds(250);
    }
    plan.reason = "profile_public_failures_short_direct";
    prefer_local_before_public(plan);
  }

  if (snapshot.profile.last_path == "relay" && snapshot.profile.path_streak >= 2 && !plan.skip_direct &&
      (plan.reason == "default" || plan.reason == "profile_public_failures_short_direct")) {
    plan.direct_timeout = std::chrono::milliseconds(600);
    plan.direct_connect = std::chrono::milliseconds(220);
    plan.reason = "profile_relay_history_short_direct";
  }
}

RoutePlan build_base_route_plan(const ConnectivitySnapshot& snapshot, const std::optional<StunProbeResult>& stun,
                                bool force_no_direct, int default_connections) {
  RoutePlan plan;
  plan.connections = default_connections;
  plan.reason = "default";
  fill_candidate_score_hints(plan, snapshot);

  if (force_no_direct || snapshot.no_direct_config) {
    plan.skip_direct = true;
    plan.direct_timeout = std::chrono::milliseconds(0);
    plan.reason = "no_direct";
    return plan;
  }

  if (snapshot.only_local) {
    plan.reason = "only_local";
  }

  if (snapshot.vpn_detected && snapshot.lan_discovered_count > 0) {
    plan.direct_timeout = std::chrono::milliseconds(1000);
    plan.direct_connect = std::chrono::milliseconds(250);
    plan.reason = "vpn_lan_short_direct";
  }

  if (stun && stun->ok) {
    if (stun->nat_class == StunNatClass::Symmetric) {
      plan.direct_timeout = kRelayStandbyDirectWindow;
      plan.direct_connect = kRelayStandbyConnectWindow;
      plan.reason = "stun_symmetric_short_direct";
    } else if (stun->nat_class == StunNatClass::Open) {
      plan.direct_timeout = std::chrono::milliseconds(3500);
      plan.reason = "stun_open";
    }
  }

  const bool both_global_ipv6 = snapshot.self_global_ipv6_count > 0 && snapshot.peer_global_ipv6_count > 0;
  if (both_global_ipv6) {
    plan.direct_timeout = std::chrono::milliseconds(3500);
    plan.direct_connect = std::chrono::milliseconds(450);
    plan.reason = "ipv6_global_direct";
    prefer_global_ipv6(plan);
  }

  if (!both_global_ipv6 && snapshot.self_nat == NatType::BehindNat && snapshot.peer_nat == NatType::BehindNat) {
    if (plan.direct_timeout > kRelayStandbyDirectWindow) {
      plan.direct_timeout = kRelayStandbyDirectWindow;
    }
    plan.direct_connect = kRelayStandbyConnectWindow;
    if (plan.reason == "default") plan.reason = "double_nat_short_punch";
  }

  if (!plan.skip_direct) {
    const StunNatClass nat = stun && stun->ok ? stun->nat_class : snapshot.stun_nat;
    if (nat == StunNatClass::Cone) {
      // UDP is currently a probe signal only. Do not promise UDP-assisted TCP
      // punching until we can coordinate a real same-path TCP punch strategy.
      if (plan.reason == "default") plan.reason = "stun_cone_direct_probe";
    }
  }

  return plan;
}

}  // namespace

RoutePlan build_route_plan(bool no_direct, const ConnectivitySnapshot& snapshot,
                           const std::optional<StunProbeResult>& stun, int connections) {
  auto plan = build_base_route_plan(snapshot, stun, no_direct, connections);
  apply_profile_route_history(plan, snapshot);
  return plan;
}

std::string describe_route_plan(const RoutePlan& plan, bool include_same_port) {
  std::string line = plan.reason;
  if (plan.skip_direct) return line + " (skip direct)";
  line += " direct_window=" + std::to_string(plan.direct_timeout.count()) + "ms";
  line += " direct_connect=" + std::to_string(plan.direct_connect.count()) + "ms";
  if (include_same_port) {
    line += " same_port=" + std::to_string(plan.same_port_timeout.count()) + "ms/" +
            std::to_string(plan.same_port_connect.count()) + "ms";
  }
  if (plan.udp_punch_enabled) line += " udp-assist";
  return line;
}

void apply_direct_candidate_scoring(std::vector<DirectCandidate>& candidates,
                                    const RoutePlan::DirectCandidateScoreHints& hints,
                                    const std::vector<std::string>& kind_order) {
  if (candidates.empty()) return;

  auto rank = [&](const std::string& kind) {
    for (std::size_t i = 0; i < kind_order.size(); ++i) {
      if (kind_order[i] == kind) return static_cast<int>(i);
    }
    return static_cast<int>(kind_order.size());
  };

  for (auto& candidate : candidates) {
    if (!kind_order.empty()) {
      const auto r = rank(candidate.kind);
      if (r < static_cast<int>(kind_order.size())) {
        candidate.priority += 1000 - r * 100;
        add_direct_candidate_reason(candidate, "route_order_hint");
      }
    }

    if (!hints.profile_direct_candidate_kind.empty() && hints.profile_last_path == "direct" &&
        candidate.kind == hints.profile_direct_candidate_kind) {
      candidate.priority += 25;
      add_direct_candidate_reason(candidate, "profile_direct_success");
    }

    const auto failure = hints.profile_candidate_failures_by_kind.find(candidate.kind);
    if (failure != hints.profile_candidate_failures_by_kind.end()) {
      candidate.priority -= std::min(30, failure->second * 5);
      add_direct_candidate_reason(candidate, "profile_previous_failure");
    }

    if (hints.vpn_detected && (candidate.kind == "discovered" || candidate.kind == "lan")) {
      candidate.priority -= 10;
      add_direct_candidate_reason(candidate, "vpn_lan_caution");
    }
  }
}

void apply_route_plan_to_adaptive(const RoutePlan& plan, Role role, AdaptivePuncher& puncher,
                                  const std::vector<DirectCandidate>& candidates, const NatProfile& self,
                                  const NatProfile& peer, PunchPlan& out) {
  auto ordered = candidates;
  apply_direct_candidate_scoring(ordered, plan.candidate_score_hints, plan.direct_candidate_order);
  out = puncher.plan(role, ordered, self, peer);
  if (plan.direct_timeout.count() > 0 && plan.direct_timeout < out.total_timeout) {
    out.total_timeout = plan.direct_timeout;
  }
  if (plan.direct_connect.count() > 0) {
    out.connect_timeout = plan.direct_connect;
  }
  if (plan.same_port_timeout.count() > 0) out.same_port_timeout = plan.same_port_timeout;
  if (plan.same_port_connect.count() > 0) out.same_port_connect_timeout = plan.same_port_connect;
  tune_direct_candidate_timeouts(out);
}

void apply_peer_direct_policy(RoutePlan& plan, bool peer_no_direct) {
  if (!peer_no_direct) return;
  plan.skip_direct = true;
  plan.direct_timeout = std::chrono::milliseconds(0);
  plan.direct_connect = std::chrono::milliseconds(0);
  plan.udp_punch_enabled = false;
  plan.reason = "peer_no_direct";
}

RoutePlan resolve_route_plan(bool no_direct, const ConnectivitySnapshot& snapshot,
                             const std::optional<StunProbeResult>& stun, int connections, bool ai_route,
                             bool ai_route_plan_only, bool ai_route_connectivity_only,
                             ProgressReporter& reporter) {
  auto plan = build_route_plan(no_direct, snapshot, stun, connections);
  if (!ai_route && !ai_route_plan_only) return plan;

  const auto cfg = route_ai_config();
  if (!cfg) return plan;

  const auto ai = ai_suggest_route_plan(snapshot, *cfg);
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
  const auto cfg = route_ai_config();
  if (!cfg) return;
  const auto ai = ai_explain_diagnosis(snapshot, plan, "direct connection failed after punch attempts", *cfg);
  if (ai.ok && !ai.text.empty()) reporter.status("ai post-direct: " + ai.text);
}

}  // namespace kiko
