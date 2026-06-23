#pragma once

#include "common.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kiko {

enum class StunNatClass { Unknown, Open, Cone, Symmetric };

struct StunProbeResult {
  bool ok = false;
  StunNatClass nat_class = StunNatClass::Unknown;
  Endpoint mapped{};
  Endpoint mapped_alt{};
  std::string error;
};

struct InterfaceAddress {
  std::string name;
  std::string address;
  bool vpn = false;
  bool loopback = false;
};

[[nodiscard]] std::string stun_nat_class_name(StunNatClass type);
[[nodiscard]] bool should_run_stun_probe(bool explicit_udp_probe, bool ai_route, bool ai_route_plan_only);
[[nodiscard]] StunProbeResult probe_stun_nat(std::chrono::milliseconds timeout = std::chrono::milliseconds(800));
[[nodiscard]] bool detect_vpn_interfaces();
[[nodiscard]] bool is_vpn_interface_name(std::string_view name);
[[nodiscard]] bool is_likely_virtual_interface_name(std::string_view name);
[[nodiscard]] std::optional<std::string> choose_physical_interface_name();
[[nodiscard]] std::vector<InterfaceAddress> collect_interface_addresses();

}  // namespace kiko
