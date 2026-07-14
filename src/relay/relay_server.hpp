#pragma once

#include "core/common.hpp"
#include "core/protocol.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace kiko {

struct RelayServerConfig {
  std::string password;
  std::chrono::seconds room_ttl{std::chrono::hours(3)};
  std::chrono::seconds cleanup_interval{std::chrono::minutes(10)};
};

class BackgroundRelay {
 public:
  BackgroundRelay();
  ~BackgroundRelay();

  BackgroundRelay(const BackgroundRelay&) = delete;
  BackgroundRelay& operator=(const BackgroundRelay&) = delete;

  void start(const Endpoint& bind_addr = Endpoint{"0.0.0.0", 0},
             const RelayServerConfig& config = {});
  void stop();

  [[nodiscard]] bool running() const { return running_.load(); }
  [[nodiscard]] Endpoint local_endpoint() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::atomic<bool> running_{false};
  Endpoint bound_;
};

[[nodiscard]] bool relay_password_ok(const RelayServerConfig& config, const Message& hello);

}  // namespace kiko
