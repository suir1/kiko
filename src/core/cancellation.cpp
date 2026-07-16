#include "core/cancellation.hpp"

namespace kiko {

void TransferCancellation::request() {
  requested_.store(true);

  std::vector<SocketInterruptHandle> sockets;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    sockets.swap(sockets_);
  }

  for (const auto& socket : sockets) socket.interrupt();
}

bool TransferCancellation::requested() const { return requested_.load(); }

const std::atomic_bool* TransferCancellation::flag() const { return &requested_; }

void TransferCancellation::track(TcpSocket& socket) {
  if (!socket.valid()) return;

  std::lock_guard<std::mutex> lock(mutex_);
  if (requested_.load()) {
    socket.interrupt();
    return;
  }
  sockets_.push_back(socket.interrupt_handle());
}

const std::atomic_bool* cancellation_flag(const std::shared_ptr<TransferCancellation>& cancellation) {
  return cancellation ? cancellation->flag() : nullptr;
}

void throw_if_cancelled(const std::atomic_bool* cancel, std::string_view message) {
  if (cancel && cancel->load()) throw KikoError(std::string(message));
}

void throw_if_cancelled(const std::shared_ptr<TransferCancellation>& cancellation, std::string_view message) {
  throw_if_cancelled(cancellation_flag(cancellation), message);
}

}  // namespace kiko
