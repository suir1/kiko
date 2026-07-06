#pragma once

#include "connect/peer_session.hpp"

namespace kiko {

struct NoteConfig : PeerSessionConfig {
};

int run_note(const NoteConfig& config, ProgressReporter& reporter);

}  // namespace kiko
