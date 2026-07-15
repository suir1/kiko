#pragma once

#include "connect/connectivity.hpp"

#include <optional>
#include <string>

namespace kiko {

struct ProfileSuccess {
  std::string path;
  std::optional<PunchStats> punch;
  std::optional<OutboundHistory> outbound;
};

[[nodiscard]] std::optional<NetworkProfileEntry> load_profile(const std::string& fingerprint);
void save_profile_success(const std::string& fingerprint, const ProfileSuccess& success);
[[nodiscard]] std::optional<OutboundHistory> outbound_history_from_profile(const NetworkProfileEntry& profile);
[[nodiscard]] std::string network_fingerprint(const NetworkInterfaceInventory& interfaces);

}  // namespace kiko
