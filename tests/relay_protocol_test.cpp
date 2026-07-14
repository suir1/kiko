#include "relay/relay_protocol.hpp"

#include <cassert>
#include <iostream>

int main() {
  using namespace kiko;

  {
    Message message{"hello",
                    {{"room", "room-a"},
                     {"role", "receiver"},
                     {"listen_port", "4242"},
                     {"local_candidates", "10.0.0.2,fd00::2"},
                     {"custom", "kept"}}};
    const auto hello = decode_relay_hello(message);
    assert(hello.room == "room-a");
    assert(hello.role == Role::Receiver);
    assert(hello.listen.port == 4242);
    assert(hello.local_candidates.size() == 2);
    assert(hello.conn_index == 0);
    assert(hello.conn_count == 1);
    assert(!hello.auxiliary);
    assert(hello.extension_fields.at("custom") == "kept");

    const auto encoded = encode_relay_hello(hello);
    assert(encoded.type == "hello");
    assert(encoded.get("room") == "room-a");
    assert(encoded.get("role") == "receiver");
    assert(encoded.get("custom") == "kept");
  }

  {
    RelayHello hello;
    hello.room = "room-b";
    hello.role = Role::Sender;
    hello.conn_index = 2;
    hello.auxiliary = true;
    hello.conn_count = 4;
    hello.extension_fields = {{"room", "wrong"}, {"role", "wrong"}, {"feature", "enabled"}};

    const auto encoded = encode_relay_hello(hello);
    assert(encoded.get("room") == "room-b");
    assert(encoded.get("role") == "sender");
    assert(encoded.get("conn_index") == "2");
    assert(encoded.get("aux") == "1");
    assert(encoded.get("conn_count") == "4");
    assert(encoded.get("feature") == "enabled");
  }

  {
    bool bad_port = false;
    try {
      (void)decode_relay_hello(
          Message{"hello", {{"room", "bad-port"}, {"role", "sender"}, {"listen_port", "70000"}}});
    } catch (const KikoError&) {
      bad_port = true;
    }
    assert(bad_port);

    bool bad_connections = false;
    try {
      (void)decode_relay_hello(
          Message{"hello", {{"room", "bad-count"}, {"role", "sender"}, {"conn_count", "1000"}}});
    } catch (const KikoError&) {
      bad_connections = true;
    }
    assert(bad_connections);
  }

  {
    RelayPeerInfo peer;
    peer.peer_public = Endpoint{"203.0.113.7", 5001};
    peer.peer_listen = Endpoint{"10.0.0.7", 5002};
    peer.peer_local_candidates = {"10.0.0.7", "fd00::7"};
    peer.peer_no_direct = true;
    peer.self_public = Endpoint{"198.51.100.9", 6001};
    peer.punch_token = "12345";
    peer.route_commit_v2 = true;
    peer.file_count = 3;
    peer.total_size = 4096;
    peer.conn_count = 4;

    const auto message = encode_relay_peer_info(peer);
    assert(message.get("route_commit") == "v2");
    assert(message.get("peer_no_direct") == "1");

    const auto decoded = decode_relay_peer_info(message);
    assert(decoded.peer_public.host == peer.peer_public.host);
    assert(decoded.peer_public.port == peer.peer_public.port);
    assert(decoded.peer_listen.host == peer.peer_listen.host);
    assert(decoded.peer_listen.port == peer.peer_listen.port);
    assert(decoded.peer_local_candidates == peer.peer_local_candidates);
    assert(decoded.peer_no_direct);
    assert(decoded.self_public.host == peer.self_public.host);
    assert(decoded.self_public.port == peer.self_public.port);
    assert(decoded.punch_token == peer.punch_token);
    assert(decoded.route_commit_v2);
    assert(decoded.file_count == peer.file_count);
    assert(decoded.total_size == peer.total_size);
    assert(decoded.conn_count == peer.conn_count);
  }

  {
    const auto defaults = decode_relay_peer_info(Message{"peer", {}});
    assert(defaults.peer_public.port == 0);
    assert(defaults.peer_listen.port == 0);
    assert(defaults.self_public.port == 0);
    assert(defaults.conn_count == 1);

    bool bad_port = false;
    try {
      (void)decode_relay_peer_info(Message{"peer", {{"peer_public_port", "70000"}}});
    } catch (const KikoError&) {
      bad_port = true;
    }
    assert(bad_port);
  }

  std::cout << "PASS: relay hello and peer-info wire contracts\n";
  return 0;
}
