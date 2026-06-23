#include "pake.hpp"
#include "net/socket.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace kiko;

namespace {

// Runs a sender/receiver handshake over loopback and reports the outcome.
// Returns true if both sides agreed on the same session key.
bool run_pair(const std::string& sender_code, const std::string& receiver_code, bool& threw) {
  threw = false;
  auto listener = TcpListener::bind(Endpoint{"127.0.0.1", 0});
  auto endpoint = listener.local_endpoint();

  SessionKey sender_key{};
  SessionKey receiver_key{};
  bool sender_threw = false;
  bool receiver_threw = false;

  std::thread sender([&] {
    try {
      auto socket = connect_tcp(endpoint, std::chrono::seconds(2));
      if (!socket.valid()) throw std::runtime_error("connect failed");
      sender_key = perform_handshake(socket, Role::Sender, sender_code);
    } catch (const std::exception&) {
      sender_threw = true;
    }
  });

  auto accepted = listener.accept(std::chrono::seconds(2));
  try {
    if (!accepted.valid()) throw std::runtime_error("accept failed");
    receiver_key = perform_handshake(accepted, Role::Receiver, receiver_code);
  } catch (const std::exception&) {
    receiver_threw = true;
  }
  sender.join();

  threw = sender_threw || receiver_threw;
  if (threw) return false;
  return sender_key == receiver_key;
}

}  // namespace

int main() {
  bool threw = false;

  bool agreed = run_pair("room-alpha", "room-alpha", threw);
  if (!agreed || threw) {
    std::cerr << "FAIL: matching codes should agree on a key (agreed=" << agreed << " threw=" << threw << ")\n";
    return 1;
  }
  std::cout << "PASS: matching codes derive identical session key\n";

  // Same rendezvous room label, different PAKE secret: must be rejected.
  bool agreed2 = run_pair("room-alpha", "room-beta", threw);
  if (agreed2 || !threw) {
    std::cerr << "FAIL: same room with wrong secret should be rejected (agreed=" << agreed2 << " threw=" << threw << ")\n";
    return 1;
  }
  std::cout << "PASS: wrong PAKE secret rejected by key confirmation\n";

  return 0;
}
