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
    report.plan.connections = 4;
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
    assert(j["plan"]["direct_connect_ms"] == 220);
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
    assert(lines.size() == 4);
    assert(lines[0].find("relay_reachable=true outbound=physical/en0 reason=physical_lower_rtt") != std::string::npos);
    assert(lines[1].find("direct_probe will_attempt=false") != std::string::npos);
    assert(lines[2].find("hint path=relay reason=direct_skipped") != std::string::npos);
    assert(lines[3].find("recommendation=relay_only") != std::string::npos);
  }

  std::cout << "PASS: doctor JSON exposes route hints and recommendations\n";
  return 0;
}
