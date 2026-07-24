#include "relay.hpp"

#include "connect/discovery.hpp"
#include "relay_server.hpp"

#include <atomic>
#include <iostream>
#include <thread>

namespace kiko {

int run_relay(const Endpoint& listen, const RelayServerConfig& config, bool announce_lan) {
  BackgroundRelay relay;
  relay.start(listen, config);
  const auto bound = relay.local_endpoint();
  std::cout << "relay listening on " << bound.to_string();
  if (!config.password.empty()) std::cout << " (password required)";
  std::cout << "\n" << std::flush;

  std::atomic<bool> stop_lan{false};
  std::thread lan_thread;
  if (announce_lan && bound.port > 0) {
    lan_thread = std::thread([&]() { lan_announce(bound.port, stop_lan); });
  }

  while (relay.running()) {
    std::this_thread::sleep_for(std::chrono::seconds(3600));
  }

  stop_lan.store(true);
  if (lan_thread.joinable()) lan_thread.join();
  return 0;
}

}  // namespace kiko
