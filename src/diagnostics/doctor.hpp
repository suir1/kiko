#pragma once

#include "connect/connectivity.hpp"
#include "outbound_policy.hpp"
#include "core/proxy.hpp"

#include <optional>
#include <string>
#include <vector>

namespace kiko {

struct DoctorOptions {
  Endpoint relay{"127.0.0.1", 9000};
  bool udp_probe = false;
  bool json_output = false;
  bool ai_explain = false;
  std::optional<ProxyConfig> proxy;
  std::optional<std::string> relay_pass;
  std::string bind_interface;
  bool avoid_vpn = false;
  bool no_direct = false;
  bool only_local = false;
};

struct RouteProbe {
  bool ok = false;
  bool routed_via_vpn = false;
  std::string target;
  std::string interface_name;
  std::string gateway;
  std::string source;
};

struct DoctorReport {
  ConnectivitySnapshot snapshot;
  std::optional<StunProbeResult> stun;
  RoutePlan plan;
  std::string diagnosis;
  std::vector<InterfaceAddress> interfaces;
  RouteProbe relay_route;
  std::string bound_interface;
  RouteProbe bound_route;
  std::string outbound_path;
  std::string outbound_reason;
  std::vector<OutboundProbe> outbound_probes;
};

[[nodiscard]] DoctorReport run_doctor(const DoctorOptions& options);
[[nodiscard]] std::string doctor_report_to_json(const DoctorReport& report);
[[nodiscard]] std::vector<std::string> doctor_debug_lines(const DoctorReport& report);

int run_doctor_cli(const DoctorOptions& options);

}  // namespace kiko
