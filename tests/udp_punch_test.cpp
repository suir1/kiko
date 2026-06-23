#include "connect/udp_punch.hpp"

#include "platform/platform.hpp"

#include <cassert>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

namespace {

bool wait_for_udp(int fd, int timeout_ms) {
  return kiko::net_poll(fd, true, false, timeout_ms) > 0;
}

}  // namespace

int main() {
  using namespace kiko;

  {
    UdpPunchParams params;
    params.peer_wan = Endpoint{"127.0.0.1", 1};
    params.token = "not-a-number";
    udp_punch_burst(params);
  }

  {
    std::atomic<bool> got{false};
    std::thread receiver([&]() {
      net_startup();
      const int fd = static_cast<int>(socket(AF_INET, SOCK_DGRAM, 0));
      if (fd < 0) return;
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      addr.sin_port = 0;
      if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        net_close(fd);
        return;
      }
      socklen_t len = sizeof(addr);
      getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
      const std::uint16_t port = ntohs(addr.sin_port);

      UdpPunchParams params;
      params.peer_wan = Endpoint{"127.0.0.1", port};
      params.token = std::to_string(now_ms() + 20);
      params.window = std::chrono::milliseconds(200);
      std::thread puncher([&]() { udp_punch_burst(params); });

      char buf[32]{};
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (std::chrono::steady_clock::now() < deadline) {
        if (!wait_for_udp(fd, 50)) continue;
        const auto n = recvfrom(fd, buf, sizeof(buf), 0, nullptr, nullptr);
        if (n > 0) {
          got.store(true);
          break;
        }
      }
      if (puncher.joinable()) puncher.join();
      net_close(fd);
    });
    if (receiver.joinable()) receiver.join();
    if (!got.load()) {
      std::cerr << "FAIL: loopback udp punch did not deliver a packet\n";
      return 1;
    }
  }

  std::cout << "udp_punch_test ok\n";
  return 0;
}
