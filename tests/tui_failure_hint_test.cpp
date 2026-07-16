#include "tui/tui_failure_hint.hpp"

#include <iostream>

int main() {
  using namespace kiko;

  TuiMenuState menu;

  {
    TuiState state;
    state.error = "direct failed after punch attempts";
    state.doctor_summary = "symmetric NAT detected";
    const auto blocked = suggest_failure_recovery(state, menu);
    if (blocked.preset != 2) {
      std::cerr << "FAIL: expected relay-only preset for symmetric NAT\n";
      return 1;
    }
  }

  {
    TuiState state;
    state.error = "relay route goes through VPN/TUN interface utun4";
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
    state.error = "relay connection refused";
    state.doctor_summary = "cannot reach relay";
    const auto relay = suggest_failure_recovery(state, menu);
    if (relay.preset != 3) {
      std::cerr << "FAIL: expected debug preset for relay errors\n";
      return 1;
    }
  }

  std::cout << "tui_failure_hint_test ok\n";
  return 0;
}
