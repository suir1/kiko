#pragma once

#include "core/adaptive.hpp"
#include "core/protocol.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace kiko {

inline constexpr std::uint64_t kMaxRelayConnections = 32;

struct RelayHello {
  std::string room;
  Role role = Role::Sender;
  std::uint64_t conn_index = 0;
  Endpoint listen;
  Endpoint punch_public;
  std::uint64_t file_count = 0;
  std::uint64_t total_size = 0;
  std::uint64_t conn_count = 1;
  bool auxiliary = false;
  bool no_direct = false;
  std::vector<std::string> local_candidates;
  std::string app;
  std::string stun_nat;
  std::optional<std::string> relay_pass;
  std::map<std::string, std::string> extension_fields;
};

struct RelayPeerInfo {
  Endpoint peer_public;
  Endpoint peer_listen;
  std::vector<std::string> peer_local_candidates;
  bool peer_no_direct = false;
  Endpoint self_public;
  std::string punch_token;
  bool route_commit_v2 = false;
  std::uint64_t file_count = 0;
  std::uint64_t total_size = 0;
  std::uint64_t conn_count = 1;
};

[[nodiscard]] Message encode_relay_hello(const RelayHello& hello);
[[nodiscard]] RelayHello decode_relay_hello(const Message& message);
[[nodiscard]] Message encode_relay_peer_info(const RelayPeerInfo& peer);
[[nodiscard]] RelayPeerInfo decode_relay_peer_info(const Message& message);

}  // namespace kiko
