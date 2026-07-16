#include "tui_failure_hint.hpp"

#include "tui_advanced.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace kiko {
namespace {

std::string to_lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool contains_ci(const std::string& haystack, const char* needle) {
  return to_lower(haystack).find(needle) != std::string::npos;
}

}  // namespace

FailureRecoveryHint suggest_failure_recovery(const TuiState& state, const TuiMenuState& menu) {
  std::string context = state.error;
  context.push_back('\n');
  context += state.doctor_summary;
  context.push_back('\n');
  context += state.route_summary;
  context.push_back('\n');
  context += state.joined_logs();
  context.push_back('\n');
  context += state.route_plan_summary;

  FailureRecoveryHint hint;
  hint.preset = 2;
  hint.reason = "Try preset 「公司 / 仅 relay」: skip direct and disable LAN discovery";

  if (contains_ci(context, "vpn") || contains_ci(context, "tun") || contains_ci(context, "wireguard")) {
    hint.preset = 2;
    hint.avoid_vpn = true;
    hint.reason = "VPN/TUN detected — use relay-only and avoid VPN interface";
    return hint;
  }

  if (contains_ci(context, "ap isol") || contains_ci(context, "symmetric") || contains_ci(context, "direct failed") ||
      contains_ci(context, "punch") || contains_ci(context, "cgnat")) {
    hint.preset = 2;
    hint.reason = "Direct path likely blocked — use relay-only (no direct, no LAN discovery)";
    return hint;
  }

  if (contains_ci(context, "relay") &&
      (contains_ci(context, "refused") || contains_ci(context, "unreachable") || contains_ci(context, "timed out") ||
       contains_ci(context, "timeout") || contains_ci(context, "no route"))) {
    hint.preset = 3;
    hint.reason = "Relay reachability issue — try debug preset (UDP probe) and check network";
    return hint;
  }

  if (contains_ci(context, "waiting for peer") || contains_ci(context, "rendezvous") ||
      contains_ci(context, "no peer")) {
    hint.preset = 0;
    hint.reason = "Peer not found — verify pairing code and that both sides use the same relay";
    return hint;
  }

  if (menu.network.preset == 2 && menu.network.no_direct && !menu.network.lan_discover) {
    hint.preset = 3;
    hint.reason = "Relay-only already active — try debug preset (UDP probe) for more detail";
  }

  return hint;
}

void apply_failure_recovery(TuiMenuState& menu, const FailureRecoveryHint& hint) {
  apply_network_preset(hint.preset, menu.network);
  if (hint.avoid_vpn) menu.network.avoid_vpn = true;
  menu.connections_text = std::to_string(menu.network.connections);
}

}  // namespace kiko
