#pragma once

#include "core/common.hpp"
#include "core/proxy.hpp"
#include "core/socket.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace kiko {

struct OutboundProbe {
  std::string path;  // default | physical | forced
  std::string bind_interface;
  std::int64_t rtt_ms = -1;
  bool pong_ok = false;
};

struct OutboundSelection {
  ConnectOptions connect_options;
  std::string chosen_path = "default";
  std::string reason = "default";
  std::vector<OutboundProbe> probes;
};

struct OutboundHistory {
  std::string path;
  std::string bind_interface;
  std::string reason;
  std::map<std::string, std::int64_t> rtt_by_path;
};

[[nodiscard]] bool relay_target_is_local(const Endpoint& relay);

[[nodiscard]] OutboundSelection select_outbound_for_relay(const Endpoint& relay,
                                                          const std::optional<ProxyConfig>& proxy,
                                                          const std::string& bind_interface,
                                                          bool avoid_vpn,
                                                          const std::optional<OutboundHistory>& history = std::nullopt);

}  // namespace kiko
