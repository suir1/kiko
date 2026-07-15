#include "ai_advisor.hpp"

#include "core/adaptive.hpp"

#include <algorithm>
#include <sstream>

namespace kiko {
namespace {

std::string nat_type_json(NatType type) {
  switch (type) {
    case NatType::Open:
      return "open";
    case NatType::BehindNat:
      return "behind-nat";
    default:
      return "unknown";
  }
}

nlohmann::json route_plan_json(const RoutePlan& plan) {
  nlohmann::json j = {{"skip_direct", plan.skip_direct},
                      {"udp_punch_enabled", plan.udp_punch_enabled},
                      {"direct_timeout_ms", plan.direct_timeout.count()},
                      {"direct_connect_ms", plan.direct_connect.count()},
                      {"same_port_timeout_ms", plan.same_port_timeout.count()},
                      {"same_port_connect_ms", plan.same_port_connect.count()},
                      {"connections", plan.connections},
                      {"reason", plan.reason}};
  if (!plan.direct_candidate_order.empty()) j["direct_candidate_order"] = plan.direct_candidate_order;
  if (!plan.relay_order.empty()) j["relay_order"] = plan.relay_order;
  return j;
}

std::string extract_json_object(const std::string& text) {
  const auto start = text.find('{');
  const auto end = text.rfind('}');
  if (start == std::string::npos || end == std::string::npos || end <= start) return text;
  return text.substr(start, end - start + 1);
}

bool valid_relay_kind(const std::string& kind) {
  return kind == "embedded" || kind == "lan" || kind == "external";
}

bool valid_direct_candidate_kind(const std::string& kind) {
  return kind == "manual" || kind == "discovered" || kind == "lan" || kind == "listen" ||
         kind == "ipv6_global" || kind == "public";
}

}  // namespace

std::string connectivity_snapshot_to_json(const ConnectivitySnapshot& snapshot) {
  nlohmann::json j;
  j["nat_self"] = nat_type_json(snapshot.self_nat);
  j["nat_peer"] = nat_type_json(snapshot.peer_nat);
  j["stun_nat_class"] = stun_nat_class_name(snapshot.stun_nat);
  j["lan_discovered_count"] = snapshot.lan_discovered_count;
  j["vpn_detected"] = snapshot.vpn_detected;
  j["only_local"] = snapshot.only_local;
  j["no_direct_config"] = snapshot.no_direct_config;
  j["lan_candidate_count"] = snapshot.lan_candidates.size();
  j["ipv6"] = {{"self_global_count", snapshot.self_global_ipv6_count},
               {"peer_global_count", snapshot.peer_global_ipv6_count}};
  j["transfer"] = {{"total_bytes", snapshot.total_bytes},
                   {"file_count", snapshot.file_count},
                   {"largest_file_bytes", snapshot.largest_file_bytes},
                   {"compressible_ratio", snapshot.compressible_ratio},
                   {"connections_hint", snapshot.connections_hint}};
  const auto& history = snapshot.profile;
  if (!history.last_path.empty() || !history.last_direct_candidate_kind.empty() ||
      !history.outbound_history.path.empty() || !history.outbound_history.rtt_by_path.empty() ||
      !history.candidate_failures_by_kind.empty()) {
    nlohmann::json profile;
    profile["last_path"] = history.last_path;
    profile["success_count"] = history.success_count;
    if (!history.outbound_history.path.empty()) {
      nlohmann::json relay;
      relay["path"] = history.outbound_history.path;
      if (!history.outbound_history.bind_interface.empty()) {
        relay["interface"] = history.outbound_history.bind_interface;
      }
      if (!history.outbound_history.reason.empty()) relay["reason"] = history.outbound_history.reason;
      if (!history.outbound_history.rtt_by_path.empty()) {
        relay["rtt_by_path"] = nlohmann::json::object();
        for (const auto& [path, rtt] : history.outbound_history.rtt_by_path) {
          relay["rtt_by_path"][path] = rtt;
        }
      }
      profile["relay_outbound"] = std::move(relay);
    }
    if (!history.last_direct_candidate_kind.empty()) {
      profile["last_direct_candidate_kind"] = history.last_direct_candidate_kind;
      if (history.last_direct_rtt_ms >= 0) profile["last_direct_rtt_ms"] = history.last_direct_rtt_ms;
    }
    if (!history.candidate_failures_by_kind.empty()) {
      profile["candidate_failures_by_kind"] = nlohmann::json::object();
      for (const auto& [kind, count] : history.candidate_failures_by_kind) {
        profile["candidate_failures_by_kind"][kind] = count;
      }
    }
    if (history.same_port_attempts > 0 || history.same_port_successes > 0 ||
        history.same_port_failure_streak > 0 || history.same_port_last_elapsed_ms >= 0) {
      profile["same_port"] = {{"attempts", history.same_port_attempts},
                              {"successes", history.same_port_successes},
                              {"failure_streak", history.same_port_failure_streak},
                              {"last_elapsed_ms", history.same_port_last_elapsed_ms}};
    }
    j["profile"] = std::move(profile);
  }
  j["relays"] = nlohmann::json::array();
  for (const auto& relay : snapshot.relays) {
    j["relays"].push_back(
        {{"kind", relay.kind}, {"endpoint", relay.endpoint}, {"rtt_ms", relay.rtt_ms}, {"pong_ok", relay.pong_ok}});
  }
  if (snapshot.punch.attempted || snapshot.punch.direct_ok || !snapshot.punch.failures.empty()) {
    nlohmann::json punch;
    punch["attempted"] = snapshot.punch.attempted;
    punch["direct_ok"] = snapshot.punch.direct_ok;
    punch["failures"] = nlohmann::json::object();
    for (const auto& [reason, count] : snapshot.punch.failures) {
      punch["failures"][reason] = count;
    }
    if (!snapshot.punch.successful_candidate_kind.empty()) {
      punch["successful_candidate_kind"] = snapshot.punch.successful_candidate_kind;
      if (!snapshot.punch.successful_candidate_endpoint.empty()) {
        punch["successful_candidate_endpoint"] = snapshot.punch.successful_candidate_endpoint;
      }
      if (!snapshot.punch.successful_candidate_family.empty()) {
        punch["successful_candidate_family"] = snapshot.punch.successful_candidate_family;
      }
      if (!snapshot.punch.successful_candidate_scope.empty()) {
        punch["successful_candidate_scope"] = snapshot.punch.successful_candidate_scope;
      }
      punch["successful_candidate_priority"] = snapshot.punch.successful_candidate_priority;
      if (snapshot.punch.successful_elapsed_ms >= 0) punch["successful_elapsed_ms"] = snapshot.punch.successful_elapsed_ms;
    }
    if (!snapshot.punch.candidate_failures_by_kind.empty()) {
      punch["candidate_failures_by_kind"] = nlohmann::json::object();
      for (const auto& [kind, count] : snapshot.punch.candidate_failures_by_kind) {
        punch["candidate_failures_by_kind"][kind] = count;
      }
    }
    j["punch"] = std::move(punch);
  }
  return j.dump();
}

RoutePlan validate_ai_route_plan(const nlohmann::json& j) {
  RoutePlan plan;
  if (j.contains("skip_direct") && j["skip_direct"].is_boolean()) plan.skip_direct = j["skip_direct"].get<bool>();
  if (j.contains("udp_punch_enabled") && j["udp_punch_enabled"].is_boolean()) {
    plan.udp_punch_enabled = j["udp_punch_enabled"].get<bool>();
  }
  if (j.contains("direct_timeout_ms") && j["direct_timeout_ms"].is_number_integer()) {
    const auto ms = j["direct_timeout_ms"].get<std::int64_t>();
    plan.direct_timeout = std::chrono::milliseconds(std::clamp<std::int64_t>(ms, 0, 10000));
  }
  if (j.contains("direct_connect_ms") && j["direct_connect_ms"].is_number_integer()) {
    const auto ms = j["direct_connect_ms"].get<std::int64_t>();
    plan.direct_connect = std::chrono::milliseconds(std::clamp<std::int64_t>(ms, 0, 5000));
  }
  if (j.contains("connections") && j["connections"].is_number_integer()) {
    plan.connections = static_cast<int>(std::clamp<std::int64_t>(j["connections"].get<std::int64_t>(), 1, 32));
  }
  if (j.contains("direct_candidate_order") && j["direct_candidate_order"].is_array()) {
    for (const auto& item : j["direct_candidate_order"]) {
      if (!item.is_string()) continue;
      const auto kind = item.get<std::string>();
      if (valid_direct_candidate_kind(kind) &&
          std::find(plan.direct_candidate_order.begin(), plan.direct_candidate_order.end(), kind) ==
              plan.direct_candidate_order.end()) {
        plan.direct_candidate_order.push_back(kind);
      }
    }
  }
  if (j.contains("relay_order") && j["relay_order"].is_array()) {
    for (const auto& item : j["relay_order"]) {
      if (!item.is_string()) continue;
      const auto kind = item.get<std::string>();
      if (valid_relay_kind(kind)) plan.relay_order.push_back(kind);
    }
  }
  if (j.contains("reason") && j["reason"].is_string()) {
    const auto reason = j["reason"].get<std::string>();
    if (reason.size() <= 64) plan.reason = reason;
  }
  return plan;
}

RoutePlan merge_ai_route_plan(const RoutePlan& rules, const RoutePlan& ai_plan, const nlohmann::json* raw_keys) {
  RoutePlan merged = rules;
  const bool ai_requested_skip = ai_plan.skip_direct;
  // kiko is direct-preferred: AI may shorten the direct window, but only an
  // explicit rule such as --no-direct may disable direct attempts entirely.
  merged.skip_direct = rules.skip_direct;
  // Keep UDP punching under rule/feature control. In this release UDP is used
  // for STUN probing; an AI suggestion must not enable the experimental poke.
  merged.udp_punch_enabled = rules.udp_punch_enabled;
  if (!raw_keys || raw_keys->contains("direct_timeout_ms")) {
    if (ai_plan.direct_timeout.count() > 0) merged.direct_timeout = ai_plan.direct_timeout;
  }
  if (!merged.skip_direct && ai_requested_skip && merged.direct_timeout > std::chrono::milliseconds(500)) {
    merged.direct_timeout = std::chrono::milliseconds(500);
  }
  if (!raw_keys || raw_keys->contains("direct_connect_ms")) {
    if (ai_plan.direct_connect.count() > 0) merged.direct_connect = ai_plan.direct_connect;
  }
  if (!raw_keys || raw_keys->contains("connections")) {
    if (ai_plan.connections >= 1) merged.connections = ai_plan.connections;
  }
  if (!raw_keys || raw_keys->contains("direct_candidate_order")) {
    merged.direct_candidate_order = ai_plan.direct_candidate_order;
  }
  if (!raw_keys || raw_keys->contains("relay_order")) merged.relay_order = ai_plan.relay_order;
  if (!ai_plan.reason.empty()) merged.reason = "ai:" + ai_plan.reason;
  return merged;
}

std::string rule_direct_failure_hints(const ConnectivitySnapshot& snapshot, const RoutePlan& plan) {
  if (!snapshot.punch.attempted || snapshot.punch.direct_ok) return {};

  std::ostringstream oss;
  auto append = [&](const char* hint) {
    if (!oss.str().empty()) oss << " ";
    oss << hint;
  };

  int failure_count = 0;
  int connect_failed = 0;
  for (const auto& [reason, count] : snapshot.punch.failures) {
    failure_count += count;
    if (reason == "connect_failed") connect_failed += count;
  }

  if (failure_count >= 3 || connect_failed >= 2) {
    append("retry hint: direct failed repeatedly — kiko will keep the direct probe short and fall back to relay");
  } else if (failure_count > 0) {
    append("retry hint: direct punch failed — relay fallback is expected");
  }

  if (snapshot.total_bytes >= 50 * 1024 * 1024 && plan.connections < 6) {
    append("throughput hint: large payload — try --connections 6-8 or --auto-connections on retry");
  }

  if (connect_failed >= 2 && plan.direct_timeout.count() > 800) {
    append("punch hint: many connect timeouts — symmetric/double NAT may need a shorter direct window");
  }

  if (snapshot.vpn_detected && snapshot.lan_discovered_count > 0) {
    append("network hint: VPN detected with LAN relays — try --local on retry");
  }

  return oss.str();
}

AiAdvisorResult ai_explain_diagnosis(const ConnectivitySnapshot& snapshot, const RoutePlan& rules,
                                     const std::string& rule_diagnosis, const AiHttpConfig& config) {
  AiAdvisorResult result;
  if (!ai_configured(config)) {
    result.error = "AI API key not configured";
    return result;
  }

  const auto hints = rule_direct_failure_hints(snapshot, rules);

  nlohmann::json body;
  body["model"] = config.model;
  body["temperature"] = 0.2;
  body["max_tokens"] = 300;
  body["messages"] = nlohmann::json::array({
      {{"role", "system"},
       {"content",
        "You explain kiko file-transfer connectivity to a technical user. Use plain English. Do not mention pairing "
        "codes, file paths, or secrets. Keep under 120 words. Include actionable CLI flags when relevant "
        "(--no-direct, --connections, --local, --auto-connections)."}},
      {{"role", "user"},
       {"content", "Connectivity snapshot JSON:\n" + connectivity_snapshot_to_json(snapshot) + "\n\nRule-based plan:\n" +
                       route_plan_json(rules).dump() + "\n\nRule diagnosis:\n" + rule_diagnosis +
                       (hints.empty() ? "" : "\n\nStructured hints:\n" + hints) +
                       "\n\nExplain what this means and what the user should try next."}},
  });

  auto chat = ai_chat_completion(config, body.dump());
  if (!chat.ok) {
    result.error = chat.error;
    return result;
  }
  result.ok = true;
  result.text = chat.content;
  return result;
}

AiAdvisorResult ai_suggest_route_plan(const ConnectivitySnapshot& snapshot, const AiHttpConfig& config) {
  AiAdvisorResult result;
  if (!ai_configured(config)) {
    result.error = "AI API key not configured";
    return result;
  }

  nlohmann::json body;
  body["model"] = config.model;
  body["temperature"] = 0.1;
  body["max_tokens"] = 220;
  body["response_format"] = {{"type", "json_object"}};
  body["messages"] = nlohmann::json::array({
      {{"role", "system"},
       {"content",
        "You suggest a kiko RoutePlan JSON object with ONLY these keys: skip_direct (bool), udp_punch_enabled (bool), "
        "direct_timeout_ms (integer 0-10000), direct_connect_ms (integer 0-5000), connections (integer 1-32), "
        "direct_candidate_order (optional array of strings: manual, discovered, lan, listen, ipv6_global, public), "
        "relay_order (optional array of strings: embedded, lan, external — prefer lower RTT kinds first when multiple "
        "relays exist), reason (short string). Use transfer.connections_hint as a baseline unless you have strong "
        "reason to change it. kiko is direct-preferred: do not set skip_direct true unless no_direct_config is true. "
        "When symmetric NAT, VPN, or AP isolation is likely, keep a short direct_timeout_ms (300-1000) instead of "
        "skipping direct. Use profile.last_direct_candidate_kind and candidate_failures_by_kind to tune candidate "
        "expectations and direct_candidate_order. Prefer LAN-ish candidate kinds before public unless history says "
        "otherwise. Keep udp_punch_enabled false; this release uses UDP only for STUN probing. High "
        "compressible_ratio + large total_bytes may warrant more connections. Never request secrets or file paths."}},
      {{"role", "user"},
       {"content", "Connectivity snapshot JSON:\n" + connectivity_snapshot_to_json(snapshot) + "\n\nReturn JSON only."}},
  });

  auto chat = ai_chat_completion(config, body.dump());
  if (!chat.ok) {
    result.error = chat.error;
    return result;
  }

  try {
    const auto parsed = nlohmann::json::parse(extract_json_object(chat.content));
    result.parsed_plan = parsed;
    result.plan = validate_ai_route_plan(parsed);
    result.ok = true;
    result.text = chat.content;
  } catch (const std::exception& e) {
    result.error = std::string("AI route plan parse failed: ") + e.what();
  }
  return result;
}

}  // namespace kiko
