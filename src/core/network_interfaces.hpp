#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kiko {

struct InterfaceAddress {
  std::string name;
  std::string address;
  bool vpn = false;
  bool loopback = false;
};

struct NetworkInterfaceInventory {
  std::vector<InterfaceAddress> interfaces;
  bool vpn_interface_present = false;

  [[nodiscard]] std::vector<std::string> lan_candidate_addresses() const;
  [[nodiscard]] bool vpn_detected() const;
  [[nodiscard]] std::optional<std::string> preferred_physical_interface() const;
};

[[nodiscard]] bool is_vpn_interface_name(std::string_view name);
[[nodiscard]] NetworkInterfaceInventory collect_network_interface_inventory();

}  // namespace kiko
