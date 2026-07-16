#include "tui/tui_advanced.hpp"

#include <iostream>

int main() {
  using namespace kiko;

  NetworkPreferences options;
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

  RecvConfig recv;
  options.no_direct = true;
  options.lan_discover = false;
  options.only_local = true;
  options.avoid_vpn = true;
  options.manual_ip = "192.0.2.10";
  options.bind_interface = "en0";
  apply_network_options_to_peer(recv, options);
  if (!recv.no_direct || recv.lan_discover || !recv.only_local || !recv.avoid_vpn ||
      recv.manual_ip != std::optional<std::string>("192.0.2.10") || recv.bind_interface != "en0") {
    std::cerr << "FAIL: shared peer connection config mapping\n";
    return 1;
  }

  apply_network_preset(0, options);
  if (validate_network_options(options)) {
    std::cerr << "FAIL: default options should validate: " << *validate_network_options(options) << "\n";
    return 1;
  }

  options.only_local = true;
  options.disable_local = true;
  if (!validate_network_options(options)) {
    std::cerr << "FAIL: expected validation error for conflicting local flags\n";
    return 1;
  }

  std::cout << "tui_advanced_test ok\n";
  return 0;
}
