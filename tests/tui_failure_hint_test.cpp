#include "tui_failure_hint.hpp"

#include <iostream>

int main() {
  using namespace kiko;

  TuiMenuState menu;

  {
    TuiState state;
    state.error_message = "direct failed after punch attempts";
    state.doctor_summary = "symmetric NAT detected";
    const auto blocked = suggest_failure_recovery(state, menu);
    if (blocked.preset != 2) {
      std::cerr << "FAIL: expected relay-only preset for symmetric NAT\n";
      return 1;
    }
  }

  {
    TuiState state;
    state.error_message = "relay route goes through VPN/TUN interface utun4";
    const auto vpn = suggest_failure_recovery(state, menu);
    if (vpn.preset != 2 || !vpn.avoid_vpn) {
      std::cerr << "FAIL: expected relay-only avoid-vpn hint for VPN route\n";
      return 1;
    }
    apply_failure_recovery(menu, vpn);
    if (!menu.network.no_direct || menu.network.lan_discover || !menu.network.avoid_vpn ||
        menu.connections_text != "4") {
      std::cerr << "FAIL: avoid-vpn recovery did not apply expected network preset\n";
      return 1;
    }
  }

  {
    TuiState state;
    state.error_message = "relay connection refused";
    state.doctor_summary = "cannot reach relay";
    const auto relay = suggest_failure_recovery(state, menu);
    if (relay.preset != 3) {
      std::cerr << "FAIL: expected debug preset for relay errors\n";
      return 1;
    }
  }

  menu.mode = 1;
  menu.code = "abc123";
  menu.relay = "127.0.0.1:9000";
  menu.output_dir = "/tmp/out";
  apply_network_preset(2, menu.network);
  menu.network.avoid_vpn = true;
  const auto cmd = build_cli_command_from_menu(menu);
  if (cmd.find("recv") == std::string::npos || cmd.find("--no-direct") == std::string::npos ||
      cmd.find("--no-lan") == std::string::npos || cmd.find("--avoid-vpn") == std::string::npos) {
    std::cerr << "FAIL: CLI command missing expected flags: " << cmd << "\n";
    return 1;
  }

  std::cout << "tui_failure_hint_test ok\n";
  return 0;
}
