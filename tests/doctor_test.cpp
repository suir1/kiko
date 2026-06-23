#include "doctor.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <chrono>
#include <iostream>

int main() {
  using namespace kiko;

  {
    DoctorReport report;
    report.snapshot.relays.push_back(RelayProbeEntry{"external", "relay.example:9000", 35, true});
    report.plan.reason = "stun_symmetric_short_direct";
    report.plan.direct_timeout = std::chrono::milliseconds(500);
    report.plan.direct_connect = std::chrono::milliseconds(220);
    report.plan.same_port_timeout = std::chrono::milliseconds(180);
    report.plan.same_port_connect = std::chrono::milliseconds(100);
    report.plan.connections = 4;
    report.snapshot.profile_same_port_attempts = 4;
    report.snapshot.profile_same_port_failure_streak = 4;
    report.snapshot.profile_same_port_last_elapsed_ms = 91;
    report.diagnosis = "synthetic";

    const auto j = nlohmann::json::parse(doctor_report_to_json(report));
    assert(j["relay_reachable"] == true);
    assert(j["recommendation"] == "short_direct_then_relay");
    assert(j["route_result_hint"]["path"] == "direct_or_relay");
    assert(j["route_result_hint"]["direct_attempted"] == true);
    assert(j["route_result_hint"]["data_relay_required"] == false);
    assert(j["route_result_hint"]["rendezvous_relay_required"] == true);
    assert(j["direct_probe"]["will_attempt"] == true);
    assert(j["direct_probe"]["timeout_ms"] == 500);
    assert(j["direct_probe"]["connect_timeout_ms"] == 220);
    assert(j["direct_probe"]["same_port"]["policy"] == "shortened");
    assert(j["direct_probe"]["same_port"]["timeout_ms"] == 180);
    assert(j["direct_probe"]["same_port"]["connect_timeout_ms"] == 100);
    assert(j["direct_probe"]["same_port"]["profile_attempts"] == 4);
    assert(j["direct_probe"]["same_port"]["profile_failure_streak"] == 4);
    assert(j["direct_probe"]["same_port"]["profile_last_elapsed_ms"] == 91);
    assert(j["plan"]["same_port_timeout_ms"] == 180);
    assert(j["plan"]["same_port_connect_ms"] == 100);
    assert(j["plan"]["direct_connect_ms"] == 220);
  }

  {
    DoctorReport report;
    report.snapshot.relays.push_back(RelayProbeEntry{"external", "relay.example:9000", 18, true});
    report.snapshot.lan_candidates = {"2001:4860:4860::8888", "fd00::10", "2001:4860:4860::8888"};
    report.snapshot.self_global_ipv6_count = 1;
    report.plan.reason = "default";
    report.diagnosis = "synthetic";

    const auto j = nlohmann::json::parse(doctor_report_to_json(report));
    if (j["ipv6"]["direct_status"] != "waiting_for_peer_global") {
      std::cerr << "FAIL: doctor did not mark local global IPv6 as waiting for peer\n";
      return 1;
    }
    if (j["ipv6"]["self_global_candidates"].size() != 1 ||
        j["ipv6"]["self_global_candidates"][0] != "2001:4860:4860::8888") {
      std::cerr << "FAIL: doctor did not expose deduplicated global IPv6 candidates\n";
      return 1;
    }
    if (j["ipv6"]["self_unique_local_candidates"].size() != 1 ||
        j["ipv6"]["self_unique_local_candidates"][0] != "fd00::10") {
      std::cerr << "FAIL: doctor did not expose unique-local IPv6 candidates\n";
      return 1;
    }

    const auto lines = doctor_debug_lines(report);
    bool saw_ipv6_line = false;
    for (const auto& line : lines) {
      if (line.find("ipv6 status=waiting_for_peer_global self_global=1 peer_global=0 self_ula=1") !=
          std::string::npos) {
        saw_ipv6_line = true;
      }
    }
    if (!saw_ipv6_line) {
      std::cerr << "FAIL: doctor debug lines did not include IPv6 status\n";
      return 1;
    }
  }

  {
    DoctorReport report;
    report.snapshot.relays.push_back(RelayProbeEntry{"external", "relay.example:9000", 18, true});
    report.snapshot.lan_candidates = {"fd00::10"};
    report.snapshot.self_global_ipv6_count = 0;
    report.plan.reason = "default";
    report.diagnosis = "synthetic";

    const auto j = nlohmann::json::parse(doctor_report_to_json(report));
    if (j["ipv6"]["direct_status"] != "lan_only_unique_local") {
      std::cerr << "FAIL: doctor did not keep ULA IPv6 scoped to LAN\n";
      return 1;
    }
    if (j["ipv6"]["direct_note"].get<std::string>().find("not WAN direct") == std::string::npos) {
      std::cerr << "FAIL: doctor IPv6 ULA note did not explain LAN scope\n";
      return 1;
    }
  }

  {
    DoctorReport report;
    report.snapshot.relays.push_back(RelayProbeEntry{"external", "relay.example:9000", -1, false});
    report.plan.reason = "default";
    report.diagnosis = "synthetic";

    const auto j = nlohmann::json::parse(doctor_report_to_json(report));
    assert(j["relay_reachable"] == false);
    assert(j["recommendation"] == "fix_relay");
    assert(j["route_result_hint"]["path"] == "none");
    assert(j["route_result_hint"]["reason"] == "relay_unreachable");
    assert(j["route_result_hint"]["data_relay_required"] == true);
    assert(j["route_result_hint"]["rendezvous_relay_required"] == true);
  }

  {
    DoctorReport report;
    report.snapshot.relays.push_back(RelayProbeEntry{"external", "relay.example:9000", 18, true});
    report.relay_route.routed_via_vpn = true;
    report.relay_route.interface_name = "utun4";
    report.plan.reason = "default";
    report.diagnosis = "synthetic";

    const auto j = nlohmann::json::parse(doctor_report_to_json(report));
    assert(j["recommendation"] == "add_vpn_direct_rule_or_avoid_vpn");
  }

  {
    DoctorReport report;
    report.snapshot.relays.push_back(RelayProbeEntry{"external", "relay.example:9000", 18, true});
    report.outbound_path = "physical";
    report.bound_interface = "en0";
    report.outbound_reason = "physical_lower_rtt";
    report.plan.skip_direct = true;
    report.plan.reason = "no_direct";
    report.diagnosis = "synthetic";

    const auto j = nlohmann::json::parse(doctor_report_to_json(report));
    assert(j["recommendation"] == "relay_only");
    assert(j["route_result_hint"]["path"] == "relay");
    assert(j["route_result_hint"]["direct_attempted"] == false);
    const auto lines = doctor_debug_lines(report);
    if (lines.size() != 5) {
      std::cerr << "FAIL: doctor debug line count changed unexpectedly\n";
      return 1;
    }
    if (lines[0].find("relay_reachable=true outbound=physical/en0 reason=physical_lower_rtt") == std::string::npos ||
        lines[1].find("direct_probe will_attempt=false") == std::string::npos ||
        lines[1].find("same_port=500ms/160ms") == std::string::npos ||
        lines[2].find("ipv6 status=direct_disabled") == std::string::npos ||
        lines[3].find("hint path=relay reason=direct_skipped") == std::string::npos ||
        lines[4].find("recommendation=relay_only") == std::string::npos) {
      std::cerr << "FAIL: doctor debug lines did not expose route and IPv6 details\n";
      return 1;
    }
  }

  std::cout << "PASS: doctor JSON exposes route hints and recommendations\n";
  return 0;
}
