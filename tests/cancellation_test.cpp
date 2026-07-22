#include "core/cancellation.hpp"
#include "core/socket.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

int main() {
  using namespace kiko;
  using namespace std::chrono_literals;

  {
    auto cancellation = std::make_shared<TransferCancellation>();
    throw_if_cancelled(cancellation);
    cancellation->request();

    std::string message;
    try {
      throw_if_cancelled(cancellation, "session canceled");
    } catch (const KikoError& error) {
      message = error.what();
    }
    if (message != "session canceled") {
      std::cerr << "FAIL: cancellation helper did not preserve the requested error\n";
      return 1;
    }
  }

  {
    std::atomic_bool cancel{false};
    if (cancellation_requested(&cancel)) {
      std::cerr << "FAIL: cancellation query reported a false request\n";
      return 1;
    }
    if (!wait_with_cancellation(1ms, &cancel, 1ms)) {
      std::cerr << "FAIL: cancellation wait did not complete\n";
      return 1;
    }
    cancel.store(true);
    if (!cancellation_requested(&cancel)) {
      std::cerr << "FAIL: cancellation query missed a request\n";
      return 1;
    }
    if (wait_with_cancellation(20ms, &cancel, 1ms)) {
      std::cerr << "FAIL: cancellation wait ignored cancellation\n";
      return 1;
    }
  }

  auto listener = TcpListener::bind(Endpoint{"127.0.0.1", 0});
  TcpSocket client;
  std::thread connector([&] { client = connect_tcp(listener.local_endpoint(), 2s); });
  auto server = listener.accept(2s);
  connector.join();

  if (!client.valid() || !server.valid()) {
    std::cerr << "FAIL: did not create loopback socket pair\n";
    return 1;
  }

  TransferCancellation cancellation;
  cancellation.track(server);

  std::atomic_bool done{false};
  std::string outcome;
  std::thread reader([&] {
    std::uint8_t byte = 0;
    try {
      const bool ok = server.recv_exact(&byte, sizeof(byte));
      outcome = ok ? "unexpected_data" : "closed";
    } catch (const std::exception& e) {
      outcome = e.what();
    }
    done.store(true);
  });

  std::this_thread::sleep_for(50ms);
  cancellation.request();

  const auto deadline = std::chrono::steady_clock::now() + 1500ms;
  while (!done.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(10ms);
  }

  client.close();
  if (!done.load()) {
    server.close();
    reader.detach();
    std::cerr << "FAIL: cancellation did not wake blocked socket read\n";
    return 1;
  }
  reader.join();

  if (!cancellation.requested() || outcome == "unexpected_data") {
    std::cerr << "FAIL: cancellation outcome was not a closed read\n";
    return 1;
  }

  std::cout << "cancellation_test ok\n";
  return 0;
}
