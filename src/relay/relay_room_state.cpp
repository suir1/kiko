#include "relay_room_state.hpp"

#include "platform/platform.hpp"
#include "core/protocol.hpp"

namespace kiko {
namespace {

bool socket_is_dead(TcpSocket& socket) {
  const int fd = socket.fd();
  if (fd < 0) return true;
  const int poll = net_poll(fd, true, false, 0);
  if (poll < 0) return true;
  if (poll == 0) return false;

  char byte = 0;
#ifdef _WIN32
  const int rc = recv(static_cast<SOCKET>(fd), &byte, 1, MSG_PEEK);
#else
  const ssize_t rc = recv(fd, &byte, 1, MSG_PEEK);
#endif
  if (rc == 0) return true;
  if (rc > 0) return false;
  const int err = net_last_error();
  return err != kErrWouldBlock && err != kErrIntr;
}

}  // namespace

std::string relay_room_base(const std::string& match_key) {
  const auto hash = match_key.find('#');
  if (hash == std::string::npos) return match_key;
  return match_key.substr(0, hash);
}

RelayRoomState::RelayRoomState(RelayServerConfig config) : config_(std::move(config)) {}

RelayRoomPairing RelayRoomState::pair_or_wait(const std::string& match_key, const std::string& active_room,
                                              RelayWaitingPeer self, bool auxiliary) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!auxiliary && active_rooms_.count(active_room) > 0) {
    RelayRoomPairing result;
    result.kind = RelayRoomPairing::Kind::RoomFull;
    result.self = std::move(self);
    return result;
  }

  auto it = waiting_.find(match_key);
  if (it == waiting_.end()) {
    waiting_.emplace(match_key, std::move(self));
    return {};
  }

  if (it->second.role == self.role) {
    RelayRoomPairing result;
    result.kind = RelayRoomPairing::Kind::RoomFull;
    result.self = std::move(self);
    return result;
  }

  RelayRoomPairing result;
  result.kind = RelayRoomPairing::Kind::Matched;
  result.peer = std::move(it->second);
  result.self = std::move(self);
  waiting_.erase(it);
  if (!auxiliary) {
    active_rooms_.insert(active_room);
    result.claimed_active_room = true;
  }
  return result;
}

void RelayRoomState::release_active_room(const std::string& room) {
  std::lock_guard<std::mutex> lock(mutex_);
  active_rooms_.erase(room);
}

void RelayRoomState::close_waiting() {
  std::vector<TcpSocket> to_close;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [_, peer] : waiting_) {
      if (peer.socket.valid()) to_close.push_back(std::move(peer.socket));
    }
    waiting_.clear();
  }
  for (auto& socket : to_close) socket.close();
}

void RelayRoomState::purge_stale_waiting() {
  const auto now = std::chrono::steady_clock::now();
  std::vector<TcpSocket> to_close;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = waiting_.begin(); it != waiting_.end();) {
      const bool expired = config_.room_ttl.count() > 0 && now - it->second.registered_at > config_.room_ttl;
      const bool dead = socket_is_dead(it->second.socket);
      if (expired || dead) {
        if (it->second.socket.fd() >= 0) to_close.push_back(std::move(it->second.socket));
        it = waiting_.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto& sock : to_close) {
    try {
      send_message(sock, Message{"error", {{"code", "room_expired"}}});
    } catch (...) {
    }
    sock.close();
  }
}

}  // namespace kiko
