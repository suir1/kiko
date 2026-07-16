#include "connect/peer_session.hpp"

#include "connect/rendezvous_session.hpp"
#include "core/cancellation.hpp"
#include "core/crypto.hpp"
#include "core/network_interfaces.hpp"
#include "core/protocol.hpp"
#include "relay/relay_server.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <string>
#include <thread>

namespace {

using namespace kiko;

PeerSessionConfig make_config(Role role, const Endpoint& relay, const std::string& code) {
  PeerSessionConfig config;
  config.role = role;
  config.code = code;
  config.relay = relay;
  config.listen = Endpoint{"127.0.0.1", 0};
  config.no_direct = true;
  config.lan_discover = false;
  config.disable_local = true;
  config.show_qrcode = false;
  config.pair_timeout = std::chrono::seconds(3);
  return config;
}

struct CodeReporter : ProgressReporter {
  std::promise<std::string> ready;

  void code_ready(const std::string& code, bool) override { ready.set_value(code); }
};

}  // namespace

int main() {
  using namespace kiko;

  {
    NetworkInterfaceInventory interfaces{
        {{"Ethernet", "10.0.0.5", false, false},
         {"Loopback", "127.0.0.1", false, true},
         {"Virtual", "172.16.0.2", false, false}}};
    assert((local_candidates_for_listener(Endpoint{"::", 5000}, interfaces) ==
            std::vector<std::string>{"10.0.0.5", "172.16.0.2"}));
    assert((local_candidates_for_listener(Endpoint{"0.0.0.0", 5000}, interfaces) ==
            std::vector<std::string>{"10.0.0.5", "172.16.0.2"}));
    assert((local_candidates_for_listener(Endpoint{"127.0.0.1", 5000}, interfaces) ==
            std::vector<std::string>{"127.0.0.1"}));
  }

  {
    BackgroundRelay relay;
    relay.start(Endpoint{"127.0.0.1", 0});
    const auto endpoint = relay.local_endpoint();

    ProgressReporter sender_reporter;
    ProgressReporter receiver_reporter;
    auto sender_config = make_config(Role::Sender, endpoint, "r561");
    auto receiver_config = make_config(Role::Receiver, endpoint, "r561");
    receiver_config.no_direct = false;

    auto sender_future =
        std::async(std::launch::async, [&] { return open_peer_session(sender_config, sender_reporter); });
    auto receiver_future =
        std::async(std::launch::async, [&] { return open_peer_session(receiver_config, receiver_reporter); });

    auto sender = sender_future.get();
    auto receiver = receiver_future.get();

    assert(sender.code == "r561");
    assert(receiver.code == "r561");
    assert(sender.outcome.data_path == "relay");
    assert(receiver.outcome.data_path == "relay");
    assert(sender.active_relay.to_string() == endpoint.to_string());
    assert(receiver.active_relay.to_string() == endpoint.to_string());
    assert(sender.key == receiver.key);

    StreamCipher sender_cipher(sender.key, true);
    StreamCipher receiver_cipher(receiver.key, false);
    const std::string text = "peer-session-ok";
    send_frame(sender.channel, sender_cipher.encrypt(Bytes(text.begin(), text.end())));
    const auto encrypted = recv_frame(receiver.channel);
    assert(encrypted);
    const auto plaintext = receiver_cipher.decrypt(*encrypted);
    assert(std::string(plaintext.begin(), plaintext.end()) == text);

    sender.channel.close();
    receiver.channel.close();
    relay.stop();
  }

  {
    BackgroundRelay relay;
    relay.start(Endpoint{"127.0.0.1", 0});
    const auto endpoint = relay.local_endpoint();

    ProgressReporter sender_reporter;
    ProgressReporter receiver_reporter;
    auto sender_config = make_config(Role::Sender, endpoint, "r563");
    auto receiver_config = make_config(Role::Receiver, endpoint, "r563");
    sender_config.no_direct = false;
    receiver_config.no_direct = false;

    auto sender_future =
        std::async(std::launch::async, [&] { return open_peer_session(sender_config, sender_reporter); });
    auto receiver_future =
        std::async(std::launch::async, [&] { return open_peer_session(receiver_config, receiver_reporter); });

    auto sender = sender_future.get();
    auto receiver = receiver_future.get();
    assert(sender.outcome.data_path == "direct");
    assert(receiver.outcome.data_path == "direct");
    assert(sender.key == receiver.key);

    sender.channel.close();
    receiver.channel.close();
    relay.stop();
  }

  {
    BackgroundRelay relay;
    relay.start(Endpoint{"127.0.0.1", 0});
    const auto endpoint = relay.local_endpoint();

    CodeReporter sender_reporter;
    auto code_future = sender_reporter.ready.get_future();
    ProgressReporter receiver_reporter;
    auto sender_config = make_config(Role::Sender, endpoint, "");
    auto sender_future =
        std::async(std::launch::async, [&] { return open_peer_session(sender_config, sender_reporter); });

    const auto generated_code = code_future.get();
    assert(!generated_code.empty());
    auto receiver_config = make_config(Role::Receiver, endpoint, generated_code);
    auto receiver_future =
        std::async(std::launch::async, [&] { return open_peer_session(receiver_config, receiver_reporter); });

    auto sender = sender_future.get();
    auto receiver = receiver_future.get();
    assert(sender.code == generated_code);
    assert(receiver.code == generated_code);

    sender.channel.close();
    receiver.channel.close();
    relay.stop();
  }

  {
    BackgroundRelay relay;
    relay.start(Endpoint{"127.0.0.1", 0});
    const auto endpoint = relay.local_endpoint();

    ProgressReporter reporter;
    auto config = make_config(Role::Sender, endpoint, "r562");
    config.pair_timeout = std::chrono::seconds(5);
    config.cancellation = std::make_shared<TransferCancellation>();

    auto waiting = std::async(std::launch::async, [&] { return open_peer_session(config, reporter); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto cancel_start = std::chrono::steady_clock::now();
    config.cancellation->request();

    bool canceled = false;
    try {
      (void)waiting.get();
    } catch (const KikoError&) {
      canceled = true;
    }
    assert(canceled);
    assert(std::chrono::steady_clock::now() - cancel_start < std::chrono::seconds(1));
    relay.stop();
  }

  std::cout << "PASS: peer session rendezvous, route selection, and encryption\n";
  return 0;
}
