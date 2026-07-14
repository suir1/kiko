#pragma once

#include "core/common.hpp"
#include "core/network_interfaces.hpp"

#include <chrono>
#include <optional>
#include <string>

namespace kiko {

enum class StunNatClass { Unknown, Open, Cone, Symmetric };

struct StunProbeResult {
  bool ok = false;
  StunNatClass nat_class = StunNatClass::Unknown;
  Endpoint mapped{};
  Endpoint mapped_alt{};
  std::string error;
};

[[nodiscard]] std::string stun_nat_class_name(StunNatClass type);
[[nodiscard]] bool should_run_stun_probe(bool explicit_udp_probe, bool ai_route, bool ai_route_plan_only);
[[nodiscard]] StunProbeResult probe_stun_nat(std::chrono::milliseconds timeout = std::chrono::milliseconds(800));

}  // namespace kiko
