#include "cancellation.hpp"

namespace kiko {

void TransferCancellation::request() {
  requested_.store(true);

  std::vector<std::shared_ptr<asio::ip::tcp::socket>> sockets;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    sockets.reserve(sockets_.size());
    for (auto it = sockets_.begin(); it != sockets_.end();) {
      if (auto socket = it->lock()) {
        sockets.push_back(std::move(socket));
        ++it;
      } else {
        it = sockets_.erase(it);
      }
    }
  }

  for (auto& socket : sockets) {
    if (!socket || !socket->is_open()) continue;
    asio::error_code ignored;
    socket->close(ignored);
  }
}

bool TransferCancellation::requested() const { return requested_.load(); }

const std::atomic_bool* TransferCancellation::flag() const { return &requested_; }

void TransferCancellation::track(TcpSocket& socket) {
  if (!socket.valid()) return;

  if (requested()) {
    socket.close();
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  sockets_.push_back(socket.weak_handle());
  if (requested_.load()) socket.close();
}

}  // namespace kiko
