#pragma once

#include "core/socket.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

namespace kiko {

class TransferCancellation {
 public:
  void request();
  [[nodiscard]] bool requested() const;
  [[nodiscard]] const std::atomic_bool* flag() const;
  void track(TcpSocket& socket);

 private:
  mutable std::mutex mutex_;
  std::atomic_bool requested_{false};
  std::vector<SocketInterruptHandle> sockets_;
};

[[nodiscard]] const std::atomic_bool* cancellation_flag(
    const std::shared_ptr<TransferCancellation>& cancellation);
[[nodiscard]] bool cancellation_requested(const std::atomic_bool* cancel) noexcept;
[[nodiscard]] bool wait_with_cancellation(std::chrono::milliseconds delay, const std::atomic_bool* cancel,
                                           std::chrono::milliseconds poll_interval = std::chrono::milliseconds(25));
void throw_if_cancelled(const std::atomic_bool* cancel, std::string_view message = "transfer canceled");
void throw_if_cancelled(const std::shared_ptr<TransferCancellation>& cancellation,
                        std::string_view message = "transfer canceled");

}  // namespace kiko
