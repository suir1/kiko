#pragma once

// Default relay when KIKO_RELAY env is unset. Override at build time:
//   cmake -DKIKO_DEFAULT_RELAY=relay.example.com:9000 ...
#ifndef KIKO_DEFAULT_RELAY
#define KIKO_DEFAULT_RELAY "106.53.170.243:9000"
#endif

namespace kiko {

inline constexpr const char* kDefaultRelay = KIKO_DEFAULT_RELAY;

}  // namespace kiko
