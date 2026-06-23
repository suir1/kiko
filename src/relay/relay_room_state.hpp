#pragma once

#include "adaptive.hpp"
#include "common.hpp"
#include "relay_server.hpp"
#include "net/socket.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace kiko {

struct RelayWaitingPeer {
  Role role = Role::Sender;
  TcpSocket socket;
  Endpoint public_endpoint;
  Endpoint listen_endpoint;
  std::uint64_t file_count = 0;
  std::uint64_t total_size = 0;
  std::uint64_t conn_count = 1;
  bool no_direct = false;
  std::string local_candidates;
  std::chrono::steady_clock::time_point registered_at = std::chrono::steady_clock::now();
};

struct RelayRoomPairing {
  enum class Kind { Waiting, Matched, RoomFull };

  Kind kind = Kind::Waiting;
  std::optional<RelayWaitingPeer> self;
  std::optional<RelayWaitingPeer> peer;
  bool claimed_active_room = false;
};

[[nodiscard]] std::string relay_room_base(const std::string& match_key);

class RelayRoomState {
 public:
  explicit RelayRoomState(RelayServerConfig config);

  [[nodiscard]] RelayRoomPairing pair_or_wait(const std::string& match_key, const std::string& active_room,
                                              RelayWaitingPeer self, bool auxiliary);
  void release_active_room(const std::string& room);
  void close_waiting();
  void purge_stale_waiting();

 private:
  RelayServerConfig config_;
  std::mutex mutex_;
  std::map<std::string, RelayWaitingPeer> waiting_;
  std::set<std::string> active_rooms_;
};

}  // namespace kiko
