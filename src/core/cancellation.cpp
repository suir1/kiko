#include "core/cancellation.hpp"

#include <algorithm>
#include <thread>

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

bool cancellation_requested(const std::atomic_bool* cancel) noexcept { return cancel && cancel->load(); }

bool wait_with_cancellation(std::chrono::milliseconds delay, const std::atomic_bool* cancel,
                            std::chrono::milliseconds poll_interval) {
  if (delay.count() <= 0) return !cancellation_requested(cancel);
  if (poll_interval.count() <= 0) poll_interval = std::chrono::milliseconds(1);

  auto remaining = delay;
  while (remaining.count() > 0) {
    if (cancellation_requested(cancel)) return false;
    const auto slice = std::min(remaining, poll_interval);
    std::this_thread::sleep_for(slice);
    remaining -= slice;
  }
  return !cancellation_requested(cancel);
}

void throw_if_cancelled(const std::atomic_bool* cancel, std::string_view message) {
  if (cancellation_requested(cancel)) throw KikoError(std::string(message));
}

void throw_if_cancelled(const std::shared_ptr<TransferCancellation>& cancellation, std::string_view message) {
  throw_if_cancelled(cancellation_flag(cancellation), message);
}

}  // namespace kiko
