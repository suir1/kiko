#include "core/network_interfaces.hpp"

#include <cassert>
#include <iostream>
#include <vector>

int main() {
  using namespace kiko;

  NetworkInterfaceInventory inventory{
      {{"lo0", "127.0.0.1", false, true},
       {"utun4", "10.8.0.2", true, false},
       {"en0", "192.168.1.10", false, false},
       {"en0", "2001:db8::10", false, false},
       {"en0", "192.168.1.10", false, false},
       {"docker0", "172.17.0.1", false, false},
       {"eth0", "10.0.0.5", false, false}}};

  assert((inventory.non_loopback_addresses() ==
          std::vector<std::string>{"10.8.0.2", "192.168.1.10", "2001:db8::10", "172.17.0.1", "10.0.0.5"}));
  assert((inventory.lan_candidate_addresses() ==
          std::vector<std::string>{"192.168.1.10", "2001:db8::10", "172.17.0.1", "10.0.0.5"}));
  assert(inventory.vpn_detected());
  assert(inventory.preferred_physical_interface() == std::optional<std::string>{"en0"});

  NetworkInterfaceInventory virtual_only{{{"lo0", "127.0.0.1", false, true},
                                          {"utun4", "10.8.0.2", true, false},
                                          {"docker0", "172.17.0.1", false, false}}};
  assert(!virtual_only.preferred_physical_interface());

  NetworkInterfaceInventory addressless_vpn;
  addressless_vpn.vpn_interface_present = true;
  assert(addressless_vpn.vpn_detected());

  assert(is_vpn_interface_name("tun0"));
  assert(is_vpn_interface_name("wg1"));
  assert(is_vpn_interface_name("utun7"));
  assert(is_vpn_interface_name("ppp0"));
  assert(is_vpn_interface_name("ipsec0"));
  assert(!is_vpn_interface_name("en0"));

  const auto system = collect_network_interface_inventory();
  for (const auto& iface : system.interfaces) assert(!iface.address.empty());

  std::cout << "PASS: network interface inventory derivation\n";
  return 0;
}
