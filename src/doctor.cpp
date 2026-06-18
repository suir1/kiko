#include "doctor.hpp"

#include "ai_advisor.hpp"
#include "ai_client.hpp"
#include "discovery.hpp"
#include "profile.hpp"
#include "protocol.hpp"
#include "relay_race.hpp"
#include "socket.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <future>
#include <iostream>
#include <sstream>
#include <vector>

namespace kiko {
namespace {

std::string join_strings(const std::vector<std::string>& values) {
  if (values.empty()) return "(none)";
  std::string out;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i) out += ", ";
    out += values[i];
  }
  return out;
}

std::vector<std::string> interface_addresses_by_kind(const std::vector<InterfaceAddress>& interfaces, bool vpn) {
  std::vector<std::string> out;
  for (const auto& iface : interfaces) {
    if (iface.loopback || iface.vpn != vpn) continue;
    out.push_back(iface.name.empty() ? iface.address : iface.name + "=" + iface.address);
  }
  return out;
}

bool safe_route_token(const std::string& value) {
  if (value.empty()) return false;
  for (unsigned char c : value) {
    if (std::isalnum(c) || c == '.' || c == ':' || c == '-') continue;
    return false;
  }
  return true;
}

std::string command_output(const std::string& command) {
  std::string out;
#ifndef _WIN32
  FILE* pipe = popen(command.c_str(), "r");
  if (!pipe) return out;
  std::array<char, 512> buf{};
  while (fgets(buf.data(), static_cast<int>(buf.size()), pipe)) out += buf.data();
  pclose(pipe);
#else
  (void)command;
#endif
  return out;
}

RouteProbe route_to_host(const std::string& host, const std::string& interface_scope = {}) {
  RouteProbe probe;
  probe.target = host;
#ifdef _WIN32
  return probe;
#else
  if (!safe_route_token(host)) return probe;
  if (!interface_scope.empty() && !safe_route_token(interface_scope)) return probe;
#ifdef __APPLE__
  const auto output = command_output("route -n get " +
                                     (interface_scope.empty() ? std::string{} : "-ifscope " + interface_scope + " ") +
                                     host + " 2>/dev/null");
  std::istringstream lines(output);
  std::string line;
  while (std::getline(lines, line)) {
    line = trim(line);
    auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    auto key = trim(line.substr(0, colon));
    auto value = trim(line.substr(colon + 1));
    if (key == "interface") probe.interface_name = value;
    else if (key == "gateway") probe.gateway = value;
  }
#else
  const auto output = command_output("ip route get " + host + " 2>/dev/null");
  std::istringstream tokens(output);
  std::string token;
  while (tokens >> token) {
    if (token == "dev") tokens >> probe.interface_name;
    else if (token == "via") tokens >> probe.gateway;
    else if (token == "src") tokens >> probe.source;
  }
#endif
  probe.ok = !probe.interface_name.empty();
  probe.routed_via_vpn = probe.ok && is_vpn_interface_name(probe.interface_name);
  return probe;
#endif
}

std::string diagnose(const DoctorReport& report) {
  const auto& s = report.snapshot;
  const auto& plan = report.plan;

  if (report.relay_route.routed_via_vpn && !report.bound_interface.empty() && !s.relays.empty() &&
      s.relays.front().pong_ok) {
    return "Relay route normally goes through VPN/TUN interface " + report.relay_route.interface_name +
           ", but this probe succeeded while bound to " + report.bound_interface + ".";
  }
  if (report.relay_route.routed_via_vpn && report.bound_interface.empty() && !s.relays.empty() &&
      s.relays.front().pong_ok) {
    return "Relay route is going through VPN/TUN interface " + report.relay_route.interface_name +
           " and relay ping succeeded. Use --avoid-vpn or --bind-interface if you want the physical path instead.";
  }
  if (report.relay_route.routed_via_vpn && !report.bound_interface.empty() && !s.relays.empty() &&
      !s.relays.front().pong_ok) {
    if (report.bound_route.ok) {
      return "Tried binding relay traffic to " + report.bound_interface + " (scoped route via " +
             report.bound_route.gateway +
             "), but relay ping still failed. The physical path or relay port may be blocked; use a VPN DIRECT rule or "
             "a relay port reachable from the physical network.";
    }
    return "Tried binding relay traffic to " + report.bound_interface +
           ", but macOS did not provide a scoped route for the relay target. Use a VPN DIRECT rule for the relay IP.";
  }
  if (report.relay_route.routed_via_vpn) {
    return "Relay route is currently going through VPN/TUN interface " + report.relay_route.interface_name +
           ". Add a DIRECT rule for the relay IP or bind future transfers to the physical interface.";
  }
  if (plan.reason == "stun_symmetric_short_direct") {
    return "STUN indicates symmetric NAT. kiko will still try a short direct probe, then fall back to relay if needed.";
  }
  if (plan.reason == "vpn_lan_short_direct") {
    return "VPN interface detected with LAN relays available. kiko will try direct briefly; --local may be faster on the same LAN.";
  }
  if (s.lan_discovered_count == 0 && s.vpn_detected) {
    return "No LAN relays discovered and VPN is active. Ensure --relay points to a reachable server or use --local on LAN.";
  }
  if (s.lan_discovered_count == 0 && !s.only_local) {
    return "No LAN relays discovered. WAN transfer will use --relay. If peers are on same WiFi but fail, check AP isolation.";
  }
  if (plan.skip_direct) {
    return "Direct connections will be skipped (" + plan.reason + "). Relay pipe is the expected path.";
  }
  if (report.stun && report.stun->ok && report.stun->nat_class == StunNatClass::Cone) {
    return "Cone NAT detected. Direct TCP may work for some peers; relay remains fallback.";
  }
  return "Network looks compatible with direct-preferred transfer. Relay remains the fallback path.";
}

bool relay_reachable(const DoctorReport& report) {
  return !report.snapshot.relays.empty() && report.snapshot.relays.front().pong_ok;
}

std::string recommendation_for(const DoctorReport& report) {
  if (!relay_reachable(report)) return "fix_relay";
  if (report.relay_route.routed_via_vpn && report.bound_interface.empty()) return "add_vpn_direct_rule_or_avoid_vpn";
  if (report.relay_route.routed_via_vpn && !report.bound_interface.empty()) return "use_bound_physical_interface";
  if (report.plan.skip_direct) return "relay_only";
  if (report.plan.reason == "stun_symmetric_short_direct" ||
      report.plan.reason == "profile_relay_history_short_direct" ||
      report.plan.reason == "double_nat_short_punch") {
    return "short_direct_then_relay";
  }
  return "direct_preferred_with_relay_fallback";
}

nlohmann::json route_result_hint_json(const DoctorReport& report) {
  if (!relay_reachable(report)) {
    return {{"path", "none"},
            {"reason", "relay_unreachable"},
            {"direct_attempted", false},
            {"data_relay_required", true},
            {"rendezvous_relay_required", true}};
  }
  if (report.plan.skip_direct) {
    return {{"path", "relay"},
            {"reason", "direct_skipped"},
            {"direct_attempted", false},
            {"data_relay_required", true},
            {"rendezvous_relay_required", true}};
  }
  return {{"path", "direct_or_relay"},
          {"reason", "direct_probe_then_relay_fallback"},
          {"direct_attempted", true},
          {"data_relay_required", false},
          {"rendezvous_relay_required", true}};
}

}  // namespace

DoctorReport run_doctor(const DoctorOptions& options) {
  DoctorReport report;
  const auto discovered = lan_discover(std::chrono::milliseconds(300));
  report.snapshot = build_pre_rendezvous_snapshot(options.no_direct, options.only_local, discovered.size(), 0);
  report.interfaces = collect_interface_addresses();
  report.relay_route = route_to_host(options.relay.host);
  const auto profile = load_profile(network_fingerprint());
  const auto outbound =
      select_outbound_for_relay(options.relay, options.proxy, options.bind_interface, options.avoid_vpn,
                                profile ? outbound_history_from_profile(*profile) : std::nullopt);
  const auto connect_options = outbound.connect_options;
  report.bound_interface = connect_options.bind_interface;
  report.outbound_path = outbound.chosen_path;
  report.outbound_reason = outbound.reason;
  report.outbound_probes = outbound.probes;
  if (!report.bound_interface.empty()) report.bound_route = route_to_host(options.relay.host, report.bound_interface);
  if (profile) apply_profile_to_snapshot(*profile, report.snapshot);
  report.snapshot.lan_discovered_count = discovered.size();

  RelayProbeEntry entry;
  entry.kind = "external";
  entry.endpoint = options.relay.to_string();
  entry.rtt_ms = probe_relay_rtt_ms(options.relay, connect_options);
  entry.pong_ok = entry.rtt_ms >= 0;
  report.snapshot.relays.push_back(entry);

  if (options.udp_probe || options.ai_explain) {
    report.stun = probe_stun_nat(std::chrono::milliseconds(800));
    if (report.stun->ok) report.snapshot.stun_nat = report.stun->nat_class;
  }

  RuleScheduler scheduler;
  report.plan = scheduler.plan(report.snapshot, report.stun, options.no_direct, 4);

  if (profile) {
    if (profile->last_path == "relay" && report.plan.reason == "default") {
      report.plan.direct_timeout = std::chrono::milliseconds(600);
      report.plan.direct_connect = std::chrono::milliseconds(220);
      report.plan.reason = "profile_relay_history_short_direct";
    }
  }

  report.diagnosis = diagnose(report);
  if (options.ai_explain) {
    auto cfg = ai_config_from_env();
    cfg.timeout = std::chrono::milliseconds(8000);
    if (ai_configured(cfg)) {
      const auto ai = ai_explain_diagnosis(report.snapshot, report.plan, report.diagnosis, cfg);
      if (ai.ok) {
        report.diagnosis = ai.text;
      } else if (!options.json_output) {
        report.diagnosis += "\n\n(AI explain unavailable: " + ai.error + ")";
      }
    }
  }
  return report;
}

std::string doctor_report_to_json(const DoctorReport& report) {
  nlohmann::json j;
  j["vpn_detected"] = report.snapshot.vpn_detected;
  j["lan_discovered_count"] = report.snapshot.lan_discovered_count;
  j["stun_nat"] = stun_nat_class_name(report.snapshot.stun_nat);
  j["relay_reachable"] = relay_reachable(report);
  j["recommendation"] = recommendation_for(report);
  j["route_result_hint"] = route_result_hint_json(report);
  j["interfaces"] = nlohmann::json::array();
  for (const auto& iface : report.interfaces) {
    if (iface.loopback) continue;
    j["interfaces"].push_back(
        {{"name", iface.name}, {"address", iface.address}, {"kind", iface.vpn ? "vpn" : "lan"}});
  }
  j["relay_route"] = {{"ok", report.relay_route.ok},
                      {"target", report.relay_route.target},
                      {"interface", report.relay_route.interface_name},
                      {"gateway", report.relay_route.gateway},
                      {"source", report.relay_route.source},
                      {"routed_via_vpn", report.relay_route.routed_via_vpn}};
  j["bound_interface"] = report.bound_interface;
  j["outbound_path"] = report.outbound_path;
  j["outbound_reason"] = report.outbound_reason;
  j["outbound_probes"] = nlohmann::json::array();
  for (const auto& probe : report.outbound_probes) {
    j["outbound_probes"].push_back({{"path", probe.path},
                                    {"bind_interface", probe.bind_interface},
                                    {"rtt_ms", probe.rtt_ms},
                                    {"pong_ok", probe.pong_ok}});
  }
  if (!report.bound_interface.empty()) {
    j["bound_route"] = {{"ok", report.bound_route.ok},
                        {"target", report.bound_route.target},
                        {"interface", report.bound_route.interface_name},
                        {"gateway", report.bound_route.gateway},
                        {"source", report.bound_route.source},
                        {"routed_via_vpn", report.bound_route.routed_via_vpn}};
  }
  j["plan"] = {{"skip_direct", report.plan.skip_direct},
               {"reason", report.plan.reason},
               {"direct_timeout_ms", report.plan.direct_timeout.count()},
               {"direct_connect_ms", report.plan.direct_connect.count()},
               {"udp_punch_enabled", report.plan.udp_punch_enabled},
               {"connections", report.plan.connections}};
  j["direct_probe"] = {{"will_attempt", !report.plan.skip_direct},
                       {"timeout_ms", report.plan.direct_timeout.count()},
                       {"connect_timeout_ms", report.plan.direct_connect.count()},
                       {"udp_assist", report.plan.udp_punch_enabled},
                       {"candidate_order", report.plan.direct_candidate_order}};
  if (!report.plan.direct_candidate_order.empty()) {
    j["plan"]["direct_candidate_order"] = report.plan.direct_candidate_order;
  }
  if (report.stun && report.stun->ok) {
    j["stun_mapped"] = report.stun->mapped.to_string();
  }
  j["relays"] = nlohmann::json::array();
  for (const auto& r : report.snapshot.relays) {
    j["relays"].push_back(
        {{"kind", r.kind}, {"endpoint", r.endpoint}, {"rtt_ms", r.rtt_ms}, {"pong_ok", r.pong_ok}});
  }
  j["diagnosis"] = report.diagnosis;
  return j.dump(2);
}

std::vector<std::string> doctor_debug_lines(const DoctorReport& report) {
  const auto hint = route_result_hint_json(report);
  std::vector<std::string> lines;
  lines.push_back("debug route: relay_reachable=" + std::string(relay_reachable(report) ? "true" : "false") +
                  " outbound=" + report.outbound_path +
                  (report.bound_interface.empty() ? std::string{} : "/" + report.bound_interface) +
                  " reason=" + report.outbound_reason);
  lines.push_back("debug route: direct_probe will_attempt=" + std::string(report.plan.skip_direct ? "false" : "true") +
                  " timeout=" + std::to_string(report.plan.direct_timeout.count()) +
                  "ms connect=" + std::to_string(report.plan.direct_connect.count()) +
                  "ms udp_assist=" + (report.plan.udp_punch_enabled ? "true" : "false"));
  lines.push_back("debug route: hint path=" + hint.value("path", std::string{}) +
                  " reason=" + hint.value("reason", std::string{}) +
                  " data_relay_required=" + (hint.value("data_relay_required", false) ? "true" : "false"));
  lines.push_back("debug route: recommendation=" + recommendation_for(report));
  return lines;
}

int run_doctor_cli(const DoctorOptions& options) {
  const auto report = run_doctor(options);
  if (options.json_output) {
    std::cout << doctor_report_to_json(report) << "\n";
  } else {
    std::cout << "kiko doctor\n";
    std::cout << "  vpn: " << (report.snapshot.vpn_detected ? "yes" : "no") << "\n";
    std::cout << "  lan ips: " << join_strings(interface_addresses_by_kind(report.interfaces, false)) << "\n";
    std::cout << "  vpn ips: " << join_strings(interface_addresses_by_kind(report.interfaces, true)) << "\n";
    if (!report.outbound_path.empty()) {
      std::cout << "  outbound: " << report.outbound_path << " (" << report.outbound_reason << ")\n";
    }
    if (!report.outbound_probes.empty()) {
      std::cout << "  outbound probes:";
      for (const auto& probe : report.outbound_probes) {
        std::cout << " " << probe.path;
        if (!probe.bind_interface.empty()) std::cout << "/" << probe.bind_interface;
        std::cout << "=" << (probe.pong_ok ? std::to_string(probe.rtt_ms) + "ms" : "fail");
      }
      std::cout << "\n";
    }
    if (!report.bound_interface.empty()) std::cout << "  bound interface: " << report.bound_interface << "\n";
    if (report.bound_route.ok) {
      std::cout << "  bound route: " << report.bound_route.interface_name;
      if (!report.bound_route.gateway.empty()) std::cout << " via " << report.bound_route.gateway;
      if (!report.bound_route.source.empty()) std::cout << " src " << report.bound_route.source;
      if (report.bound_route.routed_via_vpn) std::cout << " (vpn/tun)";
      std::cout << "\n";
    }
    if (report.relay_route.ok) {
      std::cout << "  relay route: " << report.relay_route.interface_name;
      if (!report.relay_route.gateway.empty()) std::cout << " via " << report.relay_route.gateway;
      if (!report.relay_route.source.empty()) std::cout << " src " << report.relay_route.source;
      if (report.relay_route.routed_via_vpn) std::cout << " (vpn/tun)";
      std::cout << "\n";
    }
    std::cout << "  lan relays discovered: " << report.snapshot.lan_discovered_count << "\n";
    std::cout << "  stun nat: " << stun_nat_class_name(report.snapshot.stun_nat) << "\n";
    if (report.stun && report.stun->ok) std::cout << "  stun mapped: " << report.stun->mapped.to_string() << "\n";
    for (const auto& r : report.snapshot.relays) {
      std::cout << "  relay " << r.endpoint << " ping: " << (r.pong_ok ? std::to_string(r.rtt_ms) + "ms" : "fail") << "\n";
    }
    std::cout << "  plan: skip_direct=" << (report.plan.skip_direct ? "true" : "false") << " reason=" << report.plan.reason
              << "\n";
    std::cout << "  direct probe: will_attempt=" << (report.plan.skip_direct ? "false" : "true")
              << " timeout=" << report.plan.direct_timeout.count() << "ms"
              << " connect=" << report.plan.direct_connect.count() << "ms"
              << " udp_assist=" << (report.plan.udp_punch_enabled ? "true" : "false") << "\n";
    const auto route_hint = route_result_hint_json(report);
    std::cout << "  route hint: path=" << route_hint.value("path", std::string{})
              << " reason=" << route_hint.value("reason", std::string{})
              << " data_relay_required=" << (route_hint.value("data_relay_required", false) ? "true" : "false")
              << "\n";
    std::cout << "  recommendation: " << recommendation_for(report) << "\n";
    std::cout << "\n" << report.diagnosis << "\n";
  }
  return report.snapshot.relays.empty() || !report.snapshot.relays.front().pong_ok ? 1 : 0;
}

}  // namespace kiko
