#pragma once

#include "adaptive.hpp"
#include "crypto.hpp"
#include "socket.hpp"

#include <string>

namespace kiko {

// A pairing code is split croc-style: the part before the first '-' is a public
// rendezvous label used to match peers at the relay, and the remainder is the
// secret fed into the PAKE. A code without '-' uses the whole code for both.
struct CodeParts {
  std::string room_label;
  std::string secret;
};

[[nodiscard]] CodeParts split_code(const std::string& code);

// Performs a CPace-style password-authenticated key exchange over Ristretto255
// peer channel (either a direct TCP connection or a relay-forwarded stream),
// followed by mutual key confirmation. The pairing code never leaves the peer
// and the relay cannot derive the resulting session key.
//
// Throws KikoError if confirmation fails (wrong code or active tampering).
[[nodiscard]] SessionKey perform_handshake(TcpSocket& channel, Role role, const std::string& code);

// Derives the relay matching token from the code without revealing the code.
[[nodiscard]] std::string room_token(const std::string& code);

}  // namespace kiko
