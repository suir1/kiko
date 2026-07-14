#pragma once

#include "core/common.hpp"
#include "core/proxy.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace kiko {

class TransferCancellation;

struct PeerConnectionOptions {
  Endpoint relay;
  Endpoint listen{"::", 0};
  bool no_direct = false;
  bool lan_discover = true;
  bool disable_local = false;
  bool only_local = false;
  bool udp_probe = false;
  std::optional<std::string> manual_ip;
  std::optional<ProxyConfig> proxy;
  std::optional<std::string> relay_pass;
  std::string bind_interface;
  bool avoid_vpn = false;
  std::chrono::milliseconds pair_timeout{kDefaultPairTimeout};
  std::shared_ptr<TransferCancellation> cancellation;
};

}  // namespace kiko
