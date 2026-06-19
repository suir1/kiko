#include "tui_failure_hint.hpp"

#include "tui_advanced.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
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

std::string shell_quote(const std::string& value) {
  if (value.find_first_of(" \t'\"\\") == std::string::npos) return value;
  std::string out = "'";
  for (char c : value) {
    if (c == '\'') out += "'\\''";
    else out.push_back(c);
  }
  out.push_back('\'');
  return out;
}

void append_flag(std::ostringstream& oss, const char* flag, bool enabled) {
  if (enabled) oss << ' ' << flag;
}

}  // namespace

FailureRecoveryHint suggest_failure_recovery(const TuiState& state, const TuiMenuState& menu) {
  std::string context = state.error_message;
  context.push_back('\n');
  context += state.doctor_summary;
  context.push_back('\n');
  context += state.transfer_path_summary;
  context.push_back('\n');
  context += state.connectivity_log;
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

std::string build_cli_command_from_menu(const TuiMenuState& menu) {
  std::ostringstream oss;
  const auto& net = menu.network;

  if (menu.mode == 0) {
    oss << "kiko send " << shell_quote(menu.path);
  } else {
    oss << "kiko recv " << shell_quote(menu.code) << " --out " << shell_quote(menu.output_dir);
  }

  oss << " --relay " << shell_quote(menu.relay);
  if (!menu.relay_pass.empty()) oss << " --relay-pass " << shell_quote(menu.relay_pass);
  if (!menu.code.empty() && menu.mode == 0) oss << " --code " << shell_quote(menu.code);

  append_flag(oss, "--no-direct", net.no_direct);
  append_flag(oss, "--no-lan", !net.lan_discover);
  append_flag(oss, "--local", net.only_local);
  append_flag(oss, "--no-local", net.disable_local);
  append_flag(oss, "--udp-probe", net.udp_probe);
  append_flag(oss, "--avoid-vpn", net.avoid_vpn);
  append_flag(oss, "--auto-connections", net.auto_connections);
  if (menu.mode == 0 && !net.use_gitignore) oss << " --no-gitignore";
  if (menu.mode == 0 && !net.auto_connections) oss << " --connections " << net.connections;
  if (!net.manual_ip.empty()) oss << " --ip " << shell_quote(net.manual_ip);
  if (!net.bind_interface.empty()) oss << " --bind-interface " << shell_quote(net.bind_interface);
  if (!net.proxy_url.empty()) oss << " --proxy " << shell_quote(net.proxy_url);

  return oss.str();
}

}  // namespace kiko
