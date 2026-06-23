#include "diagnostics/ai_advisor.hpp"
#include "diagnostics/ai_client.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <string>

int main() {
  using namespace kiko;

  {
    RoutePlan rules;
    rules.skip_direct = false;
    rules.direct_timeout = std::chrono::milliseconds(800);
    rules.connections = 4;
    rules.reason = "default";

    RoutePlan ai;
    ai.skip_direct = true;
    ai.udp_punch_enabled = true;
    ai.direct_timeout = std::chrono::milliseconds(0);
    ai.direct_connect = std::chrono::milliseconds(300);
    ai.connections = 8;
    ai.reason = "symmetric_nat";

    const auto merged = merge_ai_route_plan(rules, ai);
    assert(!merged.skip_direct);
    assert(!merged.udp_punch_enabled);
    assert(merged.direct_timeout.count() == 500);
    assert(merged.direct_connect.count() == 300);
    assert(merged.connections == 8);
    assert(merged.reason == "ai:symmetric_nat");
  }

  {
    RoutePlan rules;
    rules.skip_direct = true;
    rules.reason = "no_direct";
    RoutePlan ai;
    ai.skip_direct = false;
    ai.direct_timeout = std::chrono::milliseconds(2500);
    const auto merged = merge_ai_route_plan(rules, ai);
    assert(merged.skip_direct);
  }

  {
    RoutePlan rules;
    rules.udp_punch_enabled = true;
    RoutePlan ai;
    ai.skip_direct = false;
    ai.direct_candidate_order = {"lan", "listen", "public"};
    nlohmann::json raw = {{"skip_direct", false}};
    const auto merged = merge_ai_route_plan(rules, ai, &raw);
    assert(!merged.skip_direct);
    assert(merged.udp_punch_enabled);
    assert(merged.direct_candidate_order.empty());
  }

  {
    nlohmann::json j = {{"skip_direct", true},
                        {"udp_punch_enabled", true},
                        {"direct_timeout_ms", 1500},
                        {"direct_connect_ms", 400},
                        {"connections", 6},
                        {"reason", "vpn_lan"},
                        {"direct_candidate_order", nlohmann::json::array({"public", "ipv6_global", "lan", "public", "bad"})},
                        {"relay_order", nlohmann::json::array({"ignored"})}};
    const auto plan = validate_ai_route_plan(j);
    assert(plan.skip_direct);
    assert(plan.udp_punch_enabled);
    assert(plan.direct_timeout.count() == 1500);
    assert(plan.direct_connect.count() == 400);
    assert(plan.connections == 6);
    assert(plan.reason == "vpn_lan");
    assert(plan.direct_candidate_order.size() == 3);
    assert(plan.direct_candidate_order[0] == "public");
    assert(plan.direct_candidate_order[1] == "ipv6_global");
    assert(plan.direct_candidate_order[2] == "lan");
  }

  {
    RoutePlan rules;
    rules.direct_candidate_order = {"lan"};
    RoutePlan ai;
    ai.direct_candidate_order = {"listen", "public"};
    nlohmann::json raw = {{"direct_candidate_order", nlohmann::json::array({"listen", "public"})}};
    const auto merged = merge_ai_route_plan(rules, ai, &raw);
    assert(merged.direct_candidate_order.size() == 2);
    assert(merged.direct_candidate_order[0] == "listen");
    assert(merged.direct_candidate_order[1] == "public");
  }

  {
    nlohmann::json j = {{"direct_connect_ms", 99999}};
    const auto plan = validate_ai_route_plan(j);
    assert(plan.direct_connect.count() == 5000);
  }

  {
    ConnectivitySnapshot snap;
    snap.self_nat = NatType::BehindNat;
    snap.stun_nat = StunNatClass::Symmetric;
    snap.lan_discovered_count = 0;
    snap.relays.push_back(RelayProbeEntry{"external", "relay.example.com:9000", 95, true});
    snap.profile_last_path = "direct";
    snap.profile_success_count = 3;
    snap.profile_relay_path = "physical";
    snap.profile_relay_interface = "en0";
    snap.profile_relay_reason = "physical_lower_rtt";
    snap.profile_relay_rtt_by_path["default"] = 90;
    snap.profile_relay_rtt_by_path["physical"] = 42;
    snap.profile_direct_candidate_kind = "lan";
    snap.profile_direct_rtt_ms = 4;
    snap.profile_candidate_failures_by_kind["public"] = 2;
    snap.self_global_ipv6_count = 1;
    snap.peer_global_ipv6_count = 1;
    snap.punch.attempted = true;
    snap.punch.failures["connect_failed"] = 2;
    snap.punch.successful_candidate_kind = "lan";
    snap.punch.successful_candidate_priority = 90;
    snap.punch.successful_elapsed_ms = 3;
    snap.punch.candidate_failures_by_kind["public"] = 2;
    const auto json = connectivity_snapshot_to_json(snap);
    assert(json.find("behind-nat") != std::string::npos);
    assert(json.find("symmetric") != std::string::npos);
    assert(json.find("\"rtt_ms\":95") != std::string::npos);
    assert(json.find("connect_failed") != std::string::npos);
    assert(json.find("last_direct_candidate_kind") != std::string::npos);
    assert(json.find("relay_outbound") != std::string::npos);
    assert(json.find("physical_lower_rtt") != std::string::npos);
    assert(json.find("successful_candidate_kind") != std::string::npos);
    assert(json.find("candidate_failures_by_kind") != std::string::npos);
    assert(json.find("\"ipv6\"") != std::string::npos);
    assert(json.find("pairing") == std::string::npos);
  }

  {
    AiHttpConfig cfg;
    cfg.api_key = "test-key";
    cfg.timeout = std::chrono::milliseconds(50);

    cfg.base_url = "http://127.0.0.1:+123";
    auto result = ai_chat_completion(cfg, "{}");
    if (result.error.find("invalid AI base_url port") == std::string::npos) {
      std::cerr << "FAIL: AI base_url accepted signed port\n";
      return 1;
    }

    cfg.base_url = "http://127.0.0.1:70000";
    result = ai_chat_completion(cfg, "{}");
    if (result.error.find("invalid AI base_url port") == std::string::npos) {
      std::cerr << "FAIL: AI base_url accepted out-of-range port\n";
      return 1;
    }

    cfg.base_url = "http://[::1]extra";
    result = ai_chat_completion(cfg, "{}");
    if (result.error.find("invalid AI base_url host") == std::string::npos) {
      std::cerr << "FAIL: AI base_url accepted junk after bracketed IPv6 host\n";
      return 1;
    }
  }

  std::cout << "ai_test ok\n";
  return 0;
}
