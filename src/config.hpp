#pragma once

// Default relay when KIKO_RELAY env is unset. Override at build time:
//   cmake -DKIKO_DEFAULT_RELAY=relay.example.com:9000 ...
#ifndef KIKO_DEFAULT_RELAY
#define KIKO_DEFAULT_RELAY "106.53.170.243:9000"
#endif

#ifndef KIKO_VERSION
#define KIKO_VERSION "0.0.0-dev"
#endif

namespace kiko {

inline constexpr const char* kDefaultRelay = KIKO_DEFAULT_RELAY;
inline constexpr const char* kVersion = KIKO_VERSION;

}  // namespace kiko
