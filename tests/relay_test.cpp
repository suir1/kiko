#include "connect/connectivity.hpp"
#include "core/protocol.hpp"
#include "relay/relay_race.hpp"
#include "relay/relay_server.hpp"
#include "core/socket.hpp"

#include <chrono>
#include <future>
#include <iostream>
#include <thread>

int main() {
  using namespace kiko;

  RelayServerConfig config;
  config.password = "secret";

  Message no_pass{"hello", {{"room", "x"}, {"role", "sender"}}};
  Message wrong{"hello", {{"room", "x"}, {"role", "sender"}, {"relay_pass", "bad"}}};
  Message good{"hello", {{"room", "x"}, {"role", "sender"}, {"relay_pass", "secret"}}};

  if (relay_password_ok(config, no_pass)) {
    std::cerr << "FAIL: missing pass should be rejected when password configured\n";
    return 1;
  }
  if (relay_password_ok(config, wrong)) {
    std::cerr << "FAIL: wrong pass should be rejected\n";
    return 1;
  }
  if (!relay_password_ok(config, good)) {
    std::cerr << "FAIL: correct pass should be accepted\n";
    return 1;
  }

  RelayServerConfig open;
  if (!relay_password_ok(open, no_pass)) {
    std::cerr << "FAIL: open relay should accept hello without pass\n";
    return 1;
  }

  {
    BackgroundRelay relay;
    relay.start(Endpoint{"127.0.0.1", 0});
    const auto endpoint = relay.local_endpoint();

    auto client = connect_tcp(endpoint, std::chrono::seconds(2));
    if (!client.valid()) {
      std::cerr << "FAIL: bad-port client could not connect relay\n";
      return 1;
    }
    send_message(client, Message{"hello", {{"room", "bad-port"}, {"role", "sender"}, {"listen_port", "70000"}}});
    auto error = recv_message_timeout(client, std::chrono::milliseconds(500));
    if (!error || error->type != "error" || error->get("code") != "invalid_hello") {
      std::cerr << "FAIL: relay did not reject out-of-range hello port\n";
      return 1;
    }

    relay.stop();
  }

  {
    BackgroundRelay relay;
    relay.start(Endpoint{"127.0.0.1", 0});
    const auto endpoint = relay.local_endpoint();
    const std::vector<RelayRaceEntry> entries{{endpoint, false}};

    RelayHello sender_hello;
    sender_hello.room = "room-standby";
    RelayHello receiver_hello;
    receiver_hello.room = "room-standby";
    receiver_hello.role = Role::Receiver;

    auto sender_future = std::async(std::launch::async, [&] {
      return race_until_peer(entries, sender_hello, std::chrono::seconds(2), ConnectOptions{});
    });
    auto receiver_future = std::async(std::launch::async, [&] {
      return race_until_peer(entries, receiver_hello, std::chrono::seconds(2), ConnectOptions{});
    });

    auto sender_peer = sender_future.get();
    auto receiver_peer = receiver_future.get();
    if (!sender_peer || !receiver_peer) {
      std::cerr << "FAIL: standby route test did not rendezvous peers\n";
      return 1;
    }
    if (sender_peer->peer.get("route_commit") != "v2" || receiver_peer->peer.get("route_commit") != "v2") {
      std::cerr << "FAIL: relay did not advertise route commit v2\n";
      return 1;
    }

    send_message(sender_peer->socket, Message{"relay_standby", {}});
    send_message(receiver_peer->socket, Message{"relay_standby", {}});
    send_message(receiver_peer->socket, Message{"relay_ready", {}});

    auto sender_relay_start = recv_message_timeout(sender_peer->socket, std::chrono::milliseconds(800));
    auto receiver_relay_start = recv_message_timeout(receiver_peer->socket, std::chrono::milliseconds(800));
    if (!sender_relay_start || sender_relay_start->type != "relay_start" || !receiver_relay_start ||
        receiver_relay_start->type != "relay_start") {
      std::cerr << "FAIL: relay did not start from standby + relay_ready\n";
      return 1;
    }
    if (sender_relay_start->get("reason") != "standby" || receiver_relay_start->get("reason") != "standby") {
      std::cerr << "FAIL: standby relay start did not report standby reason\n";
      return 1;
    }
    sender_peer->socket.close();
    receiver_peer->socket.close();

    relay.stop();
  }

  {
    BackgroundRelay relay;
    relay.start(Endpoint{"127.0.0.1", 0});
    const auto endpoint = relay.local_endpoint();
    const std::vector<RelayRaceEntry> entries{{endpoint, false}};

    RelayHello sender_hello;
    sender_hello.room = "room-route-drop";
    RelayHello receiver_hello;
    receiver_hello.room = "room-route-drop";
    receiver_hello.role = Role::Receiver;

    auto sender_future = std::async(std::launch::async, [&] {
      return race_until_peer(entries, sender_hello, std::chrono::seconds(2), ConnectOptions{});
    });
    auto receiver_future = std::async(std::launch::async, [&] {
      return race_until_peer(entries, receiver_hello, std::chrono::seconds(2), ConnectOptions{});
    });

    auto sender_peer = sender_future.get();
    auto receiver_peer = receiver_future.get();
    if (!sender_peer || !receiver_peer) {
      std::cerr << "FAIL: route-drop test did not rendezvous peers\n";
      return 1;
    }

    send_message(sender_peer->socket, Message{"relay_standby", {}});
    receiver_peer->socket.close();

    const auto start = std::chrono::steady_clock::now();
    auto done = recv_message_timeout(sender_peer->socket, std::chrono::milliseconds(800));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    if (!done || done->type != "done") {
      std::cerr << "FAIL: live peer did not receive done after route peer disconnected\n";
      return 1;
    }
    if (elapsed > std::chrono::milliseconds(500)) {
      std::cerr << "FAIL: route peer disconnect took too long to resolve\n";
      return 1;
    }

    relay.stop();
  }

  {
    BackgroundRelay relay;
    relay.start(Endpoint{"127.0.0.1", 0});
    const auto endpoint = relay.local_endpoint();

    auto client = connect_tcp(endpoint, std::chrono::seconds(2));
    if (!client.valid()) {
      std::cerr << "FAIL: bad-conn-count client could not connect relay\n";
      return 1;
    }
    send_message(client, Message{"hello", {{"room", "bad-conn-count"}, {"role", "sender"}, {"conn_count", "1000"}}});
    auto error = recv_message_timeout(client, std::chrono::milliseconds(500));
    if (!error || error->type != "error" || error->get("code") != "invalid_hello") {
      std::cerr << "FAIL: relay did not reject excessive conn_count\n";
      return 1;
    }

    relay.stop();
  }

  {
    BackgroundRelay relay;
    relay.start(Endpoint{"127.0.0.1", 0});
    const auto endpoint = relay.local_endpoint();

    auto sender = connect_tcp(endpoint, std::chrono::seconds(2));
    if (!sender.valid()) {
      std::cerr << "FAIL: sender could not connect relay\n";
      return 1;
    }
    send_message(sender, Message{"ping", {}});
    auto sender_pong = recv_message(sender);
    if (!sender_pong || sender_pong->type != "pong") {
      std::cerr << "FAIL: sender did not receive relay pong\n";
      return 1;
    }
    send_message(sender, Message{"hello", {{"room", "room-a"}, {"role", "sender"}}});

    auto receiver = connect_tcp(endpoint, std::chrono::seconds(2));
    if (!receiver.valid()) {
      std::cerr << "FAIL: receiver could not connect relay\n";
      return 1;
    }
    send_message(receiver, Message{"ping", {}});
    auto receiver_pong = recv_message(receiver);
    if (!receiver_pong || receiver_pong->type != "pong") {
      std::cerr << "FAIL: receiver did not receive relay pong\n";
      return 1;
    }
    send_message(receiver, Message{"hello", {{"room", "room-a"}, {"role", "receiver"}}});

    auto sender_peer = recv_message(sender);
    auto receiver_peer = recv_message(receiver);
    if (!sender_peer || sender_peer->type != "peer" || !receiver_peer || receiver_peer->type != "peer") {
      std::cerr << "FAIL: relay did not rendezvous peers after ping preflight\n";
      return 1;
    }
    send_message(sender, Message{"direct_ok", {}});
    send_message(receiver, Message{"direct_ok", {}});
    auto sender_direct_start = recv_message(sender);
    auto receiver_direct_start = recv_message(receiver);
    if (!sender_direct_start || sender_direct_start->type != "direct_start" || !receiver_direct_start ||
        receiver_direct_start->type != "direct_start") {
      std::cerr << "FAIL: relay did not confirm direct route\n";
      return 1;
    }

    relay.stop();
  }

  {
    BackgroundRelay relay;
    relay.start(Endpoint{"127.0.0.1", 0});
    const auto endpoint = relay.local_endpoint();
    const std::vector<RelayRaceEntry> entries{{endpoint, false}};

    RelayHello sender_hello;
    sender_hello.room = "room-b";
    RelayHello receiver_hello;
    receiver_hello.room = "room-b";
    receiver_hello.role = Role::Receiver;

    auto sender_future = std::async(std::launch::async, [&] {
      return race_until_peer(entries, sender_hello, std::chrono::seconds(2), ConnectOptions{});
    });
    auto receiver_future = std::async(std::launch::async, [&] {
      return race_until_peer(entries, receiver_hello, std::chrono::seconds(2), ConnectOptions{});
    });

    auto sender_peer = sender_future.get();
    auto receiver_peer = receiver_future.get();
    if (!sender_peer || !receiver_peer || sender_peer->peer.type != "peer" || receiver_peer->peer.type != "peer") {
      std::cerr << "FAIL: race_until_peer did not deliver fast rendezvous peer messages\n";
      return 1;
    }
    send_message(sender_peer->socket, Message{"direct_ok", {}});
    send_message(receiver_peer->socket, Message{"direct_ok", {}});
    auto sender_direct_start = recv_message(sender_peer->socket);
    auto receiver_direct_start = recv_message(receiver_peer->socket);
    if (!sender_direct_start || sender_direct_start->type != "direct_start" || !receiver_direct_start ||
        receiver_direct_start->type != "direct_start") {
      std::cerr << "FAIL: race_until_peer did not receive direct confirmation\n";
      return 1;
    }

    relay.stop();
  }

  {
    BackgroundRelay relay;
    relay.start(Endpoint{"127.0.0.1", 0});
    const auto endpoint = relay.local_endpoint();
    const std::vector<RelayRaceEntry> entries{{endpoint, false}};

    std::uint16_t sender_port = 0;
    std::uint16_t receiver_port = 0;
    {
      auto sender_reservation = TcpListener::bind(Endpoint{"127.0.0.1", 0});
      sender_port = sender_reservation.local_endpoint().port;
    }
    {
      auto receiver_reservation = TcpListener::bind(Endpoint{"127.0.0.1", 0});
      receiver_port = receiver_reservation.local_endpoint().port;
    }

    RelayHello sender_hello;
    sender_hello.room = "room-punch-map";
    sender_hello.listen.port = sender_port;
    RelayHello receiver_hello;
    receiver_hello.room = "room-punch-map";
    receiver_hello.role = Role::Receiver;
    receiver_hello.listen.port = receiver_port;

    auto sender_future = std::async(std::launch::async, [&] {
      return race_until_peer(entries, sender_hello, std::chrono::seconds(2), ConnectOptions{});
    });
    auto receiver_future = std::async(std::launch::async, [&] {
      return race_until_peer(entries, receiver_hello, std::chrono::seconds(2), ConnectOptions{});
    });

    auto sender_peer = sender_future.get();
    auto receiver_peer = receiver_future.get();
    if (!sender_peer || !receiver_peer || sender_peer->peer.type != "peer" || receiver_peer->peer.type != "peer") {
      std::cerr << "FAIL: punch mapping peers did not rendezvous\n";
      return 1;
    }
    if (sender_peer->peer.get_u64("peer_public_port", 0) != receiver_port ||
        receiver_peer->peer.get_u64("peer_public_port", 0) != sender_port) {
      std::cerr << "FAIL: relay did not exchange punch-observed public ports\n";
      return 1;
    }
    if (sender_peer->peer.get_u64("your_public_port", 0) != sender_port ||
        receiver_peer->peer.get_u64("your_public_port", 0) != receiver_port) {
      std::cerr << "FAIL: relay did not return punch-observed self ports\n";
      return 1;
    }

    send_message(sender_peer->socket, Message{"direct_ok", {}});
    send_message(receiver_peer->socket, Message{"direct_ok", {}});
    (void)recv_message(sender_peer->socket);
    (void)recv_message(receiver_peer->socket);

    relay.stop();
  }

  {
    BackgroundRelay relay;
    relay.start(Endpoint{"127.0.0.1", 0});
    const auto endpoint = relay.local_endpoint();
    const std::vector<RelayRaceEntry> entries{{endpoint, false}};

    RelayHello sender_hello;
    sender_hello.room = "room-c";
    sender_hello.no_direct = true;
    RelayHello receiver_hello;
    receiver_hello.room = "room-c";
    receiver_hello.role = Role::Receiver;

    auto sender_future = std::async(std::launch::async, [&] {
      return race_until_peer(entries, sender_hello, std::chrono::seconds(2), ConnectOptions{});
    });
    auto receiver_future = std::async(std::launch::async, [&] {
      return race_until_peer(entries, receiver_hello, std::chrono::seconds(2), ConnectOptions{});
    });

    auto sender_peer = sender_future.get();
    auto receiver_peer = receiver_future.get();
    if (!sender_peer || !receiver_peer) {
      std::cerr << "FAIL: mismatch route test did not rendezvous peers\n";
      return 1;
    }
    if (receiver_peer->peer.get("peer_no_direct") != "1") {
      std::cerr << "FAIL: relay did not propagate peer no_direct flag\n";
      return 1;
    }
    send_message(sender_peer->socket, Message{"direct_ok", {}});
    send_message(receiver_peer->socket, Message{"relay_ready", {}});
    auto sender_relay_start = recv_message(sender_peer->socket);
    auto receiver_relay_start = recv_message(receiver_peer->socket);
    if (!sender_relay_start || sender_relay_start->type != "relay_start" || !receiver_relay_start ||
        receiver_relay_start->type != "relay_start") {
      std::cerr << "FAIL: relay did not fall back when route choices mismatched\n";
      return 1;
    }
    if (sender_relay_start->get("reason") != "mismatch" || receiver_relay_start->get("reason") != "mismatch") {
      std::cerr << "FAIL: relay fallback did not report mismatch reason\n";
      return 1;
    }
    sender_peer->socket.close();
    receiver_peer->socket.close();

    relay.stop();
  }

  {
    BackgroundRelay relay;
    relay.start(Endpoint{"127.0.0.1", 0});
    const auto endpoint = relay.local_endpoint();

    auto stale_sender = connect_tcp(endpoint, std::chrono::seconds(2));
    if (!stale_sender.valid()) {
      std::cerr << "FAIL: stale sender could not connect relay\n";
      return 1;
    }
    send_message(stale_sender, Message{"hello", {{"room", "room-d"}, {"role", "sender"}}});
    stale_sender.close();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto sender = connect_tcp(endpoint, std::chrono::seconds(2));
    auto receiver = connect_tcp(endpoint, std::chrono::seconds(2));
    if (!sender.valid() || !receiver.valid()) {
      std::cerr << "FAIL: replacement peers could not connect relay\n";
      return 1;
    }
    send_message(sender, Message{"hello", {{"room", "room-d"}, {"role", "sender"}}});
    send_message(receiver, Message{"hello", {{"room", "room-d"}, {"role", "receiver"}}});

    auto sender_peer = recv_message(sender);
    auto receiver_peer = recv_message(receiver);
    if (!sender_peer || sender_peer->type != "peer" || !receiver_peer || receiver_peer->type != "peer") {
      std::cerr << "FAIL: stale waiting peer was not purged before replacement rendezvous\n";
      return 1;
    }
    send_message(sender, Message{"direct_ok", {}});
    send_message(receiver, Message{"direct_ok", {}});
    (void)recv_message(sender);
    (void)recv_message(receiver);

    relay.stop();
  }

  std::cout << "relay_test ok\n";
  return 0;
}
