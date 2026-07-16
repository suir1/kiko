#pragma once

#include "connect/peer_session.hpp"

namespace kiko {

int run_note(const PeerSessionConfig& config, ProgressReporter& reporter);

}  // namespace kiko
