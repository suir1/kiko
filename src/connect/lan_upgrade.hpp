#pragma once

#include "core/adaptive.hpp"
#include "core/socket.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace kiko {

// Attempts a croc-style LAN upgrade over an established relay pipe before the
// end-to-end PAKE starts. Failure returns the original relay channel.
TcpSocket resolve_relay_channel(Role role, TcpSocket relay, TcpListener& listener, std::uint16_t listen_port,
                                const std::vector<std::string>& local_addrs, bool no_direct);

}  // namespace kiko
