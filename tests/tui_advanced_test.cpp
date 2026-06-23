#include "tui/tui_advanced.hpp"

#include <iostream>

int main() {
  using namespace kiko;

  TuiNetworkOptions options;
  apply_network_preset(2, options);
  if (!options.no_direct || options.lan_discover) {
    std::cerr << "FAIL: corp preset should force relay and disable LAN discovery\n";
    return 1;
  }

  apply_network_preset(3, options);
  if (!options.udp_probe) {
    std::cerr << "FAIL: debug preset should enable udp probe\n";
    return 1;
  }

  apply_network_preset(1, options);
  if (!options.auto_connections) {
    std::cerr << "FAIL: wifi preset should enable auto connections\n";
    return 1;
  }

  SendConfig send;
  apply_network_preset(0, options);
  apply_network_options_to_send(send, options);
  if (!send.lan_discover || send.no_direct || send.connections != 4) {
    std::cerr << "FAIL: send config mapping for default preset\n";
    return 1;
  }

  if (validate_network_options(options, 0)) {
    std::cerr << "FAIL: default options should validate: " << *validate_network_options(options, 0) << "\n";
    return 1;
  }

  options.only_local = true;
  options.disable_local = true;
  if (!validate_network_options(options, 0)) {
    std::cerr << "FAIL: expected validation error for conflicting local flags\n";
    return 1;
  }

  std::cout << "tui_advanced_test ok\n";
  return 0;
}
