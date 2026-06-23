#pragma once

#include "core/common.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace kiko {

inline constexpr const char* kDiscoveryMagic = "kiko";
inline constexpr std::uint16_t kDiscoveryPort = 8765;
inline constexpr const char* kMulticastV4 = "239.255.255.250";
inline constexpr const char* kMulticastV6 = "ff02::c";

// Broadcasts "kiko<port>" on LAN multicast until stop is set (runs in a thread).
void lan_announce(std::uint16_t port, std::atomic<bool>& stop);

// Listens briefly for LAN announcements; returns host:port endpoints from senders/relays.
[[nodiscard]] std::vector<Endpoint> lan_discover(std::chrono::milliseconds timeout);

}  // namespace kiko
