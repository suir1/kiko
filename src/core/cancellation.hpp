#pragma once

#include "core/socket.hpp"

#include <atomic>
#include <memory>
#include <mutex>
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
  std::vector<std::weak_ptr<asio::ip::tcp::socket>> sockets_;
};

}  // namespace kiko
