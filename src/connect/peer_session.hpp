#pragma once

#include "connect/peer_options.hpp"
#include "core/adaptive.hpp"
#include "core/crypto.hpp"
#include "core/progress.hpp"
#include "core/socket.hpp"

#include <memory>
#include <string>

namespace kiko {

class BackgroundRelay;
struct PeerSessionConfig : PeerConnectionOptions {
  Role role = Role::Sender;
  std::string code;
  bool show_qrcode = true;
  std::string app = "session";
};

struct PeerSession {
  TcpSocket channel;
  SessionKey key;
  std::string code;
  RouteOutcome outcome;
  Endpoint active_relay;
  std::shared_ptr<BackgroundRelay> relay_keepalive;
};

[[nodiscard]] PeerSession open_peer_session(const PeerSessionConfig& config, ProgressReporter& reporter);

}  // namespace kiko
