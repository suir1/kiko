#pragma once

#include "core/adaptive.hpp"
#include "core/crypto.hpp"
#include "core/progress.hpp"
#include "core/socket.hpp"

#include <string>

namespace kiko {

class TransferCancellation;

struct EncryptedSession {
  TcpSocket channel;
  SessionKey key;
  RouteTiming timing;
};

[[nodiscard]] EncryptedSession secure_encrypted_session(TcpSocket channel, Role role, const std::string& code,
                                                        const std::string& route, RouteTiming timing,
                                                        ProgressReporter& reporter,
                                                        TransferCancellation* cancellation = nullptr);

}  // namespace kiko
