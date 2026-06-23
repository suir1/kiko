#pragma once

#include "core/common.hpp"
#include "relay_server.hpp"

namespace kiko {

int run_relay(const Endpoint& listen, const RelayServerConfig& config = {});

}  // namespace kiko
