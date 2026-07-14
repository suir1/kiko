#pragma once

#include "connect/encrypted_session.hpp"
#include "connect/peer_options.hpp"
#include "core/progress.hpp"

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
  EncryptedSession encrypted;
  std::string code;
  RouteOutcome outcome;
  Endpoint active_relay;
  std::shared_ptr<BackgroundRelay> relay_keepalive;
};

[[nodiscard]] PeerSession open_peer_session(const PeerSessionConfig& config, ProgressReporter& reporter);

}  // namespace kiko
