#pragma once

#include "connect/encrypted_session.hpp"
#include "core/adaptive.hpp"
#include "core/common.hpp"
#include "core/progress.hpp"
#include "core/proxy.hpp"

#include <memory>
#include <optional>
#include <string>

namespace kiko {

class BackgroundRelay;
class TransferCancellation;

struct PeerSessionConfig {
  Role role = Role::Sender;
  std::string code;
  Endpoint relay;
  Endpoint listen{"::", 0};
  bool no_direct = false;
  bool lan_discover = true;
  bool disable_local = false;
  bool only_local = false;
  bool udp_probe = false;
  bool show_qrcode = true;
  std::optional<std::string> manual_ip;
  std::optional<ProxyConfig> proxy;
  std::optional<std::string> relay_pass;
  std::string bind_interface;
  bool avoid_vpn = false;
  std::chrono::milliseconds pair_timeout{kDefaultPairTimeout};
  std::string app = "session";
  std::shared_ptr<TransferCancellation> cancellation;
};

struct PeerSession {
  EncryptedSession encrypted;
  std::string code;
  RouteOutcome outcome;
  Endpoint active_relay;
  std::shared_ptr<BackgroundRelay> relay_keepalive;
};

[[nodiscard]] PeerSession open_peer_session(const PeerSessionConfig& config, ProgressReporter& reporter);

}  // namespace kiko
