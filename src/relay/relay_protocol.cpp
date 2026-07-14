#include "relay/relay_protocol.hpp"

#include <array>
#include <string_view>

namespace kiko {
namespace {

constexpr std::array<std::string_view, 16> kHelloFields{
    "room",                "role",           "conn_index",       "listen_host",
    "listen_port",         "punch_public_host", "punch_public_port", "file_count",
    "total_size",          "conn_count",     "aux",              "no_direct",
    "local_candidates",    "app",            "stun_nat",         "relay_pass",
};

template <std::size_t N>
std::map<std::string, std::string> extension_fields(
    const Message& message, const std::array<std::string_view, N>& protocol_fields) {
  auto fields = message.fields;
  for (const auto field : protocol_fields) fields.erase(std::string(field));
  return fields;
}

template <std::size_t N>
void clear_protocol_fields(std::map<std::string, std::string>& fields,
                           const std::array<std::string_view, N>& protocol_fields) {
  for (const auto field : protocol_fields) fields.erase(std::string(field));
}

void validate_connection_count(std::uint64_t count) {
  if (count == 0 || count > kMaxRelayConnections) throw KikoError("invalid conn_count");
}

}  // namespace

Message encode_relay_hello(const RelayHello& hello) {
  if (hello.room.empty()) throw KikoError("relay hello room is required");
  validate_connection_count(hello.conn_count);

  Message message{"hello", hello.extension_fields};
  clear_protocol_fields(message.fields, kHelloFields);
  message.fields["room"] = hello.room;
  message.fields["role"] = role_name(hello.role);
  if (hello.conn_index > 0) message.fields["conn_index"] = std::to_string(hello.conn_index);
  if (!hello.listen.host.empty()) message.fields["listen_host"] = hello.listen.host;
  if (hello.listen.port > 0) message.fields["listen_port"] = std::to_string(hello.listen.port);
  if (!hello.punch_public.host.empty()) message.fields["punch_public_host"] = hello.punch_public.host;
  if (hello.punch_public.port > 0) {
    message.fields["punch_public_port"] = std::to_string(hello.punch_public.port);
  }
  if (hello.file_count > 0) message.fields["file_count"] = std::to_string(hello.file_count);
  if (hello.total_size > 0) message.fields["total_size"] = std::to_string(hello.total_size);
  if (hello.conn_count != 1) message.fields["conn_count"] = std::to_string(hello.conn_count);
  if (hello.auxiliary) message.fields["aux"] = "1";
  if (hello.no_direct) message.fields["no_direct"] = "1";
  if (!hello.local_candidates.empty()) message.fields["local_candidates"] = join_csv(hello.local_candidates);
  if (!hello.app.empty()) message.fields["app"] = hello.app;
  if (!hello.stun_nat.empty()) message.fields["stun_nat"] = hello.stun_nat;
  if (hello.relay_pass && !hello.relay_pass->empty()) message.fields["relay_pass"] = *hello.relay_pass;
  return message;
}

RelayHello decode_relay_hello(const Message& message) {
  if (message.type != "hello") throw KikoError("expected hello");

  RelayHello hello;
  hello.room = message.get("room");
  if (hello.room.empty()) throw KikoError("relay hello room is required");
  hello.role = parse_role(message.get("role"));
  hello.conn_index = message.get_u64("conn_index", 0);
  hello.listen = Endpoint{message.get("listen_host"), message_port_or(message, "listen_port", 0, true)};
  hello.punch_public =
      Endpoint{message.get("punch_public_host"), message_port_or(message, "punch_public_port", 0, true)};
  hello.file_count = message.get_u64("file_count", 0);
  hello.total_size = message.get_u64("total_size", 0);
  hello.conn_count = message.get_u64("conn_count", 1);
  validate_connection_count(hello.conn_count);
  hello.auxiliary = message.get("aux") == "1" || hello.conn_index > 0;
  hello.no_direct = message.get("no_direct") == "1";
  hello.local_candidates = split_csv(message.get("local_candidates"));
  hello.app = message.get("app");
  hello.stun_nat = message.get("stun_nat");
  const auto relay_pass = message.get("relay_pass");
  if (!relay_pass.empty()) hello.relay_pass = relay_pass;
  hello.extension_fields = extension_fields(message, kHelloFields);
  return hello;
}

Message encode_relay_peer_info(const RelayPeerInfo& peer) {
  validate_connection_count(peer.conn_count);

  Message message{"peer", {}};
  message.fields["peer_public_host"] = peer.peer_public.host;
  message.fields["peer_public_port"] = std::to_string(peer.peer_public.port);
  message.fields["peer_listen_host"] = peer.peer_listen.host;
  message.fields["peer_listen_port"] = std::to_string(peer.peer_listen.port);
  message.fields["peer_local_candidates"] = join_csv(peer.peer_local_candidates);
  message.fields["peer_no_direct"] = peer.peer_no_direct ? "1" : "0";
  message.fields["your_public_host"] = peer.self_public.host;
  message.fields["your_public_port"] = std::to_string(peer.self_public.port);
  message.fields["punch_token"] = peer.punch_token;
  if (peer.route_commit_v2) message.fields["route_commit"] = "v2";
  message.fields["file_count"] = std::to_string(peer.file_count);
  message.fields["total_size"] = std::to_string(peer.total_size);
  message.fields["conn_count"] = std::to_string(peer.conn_count);
  return message;
}

RelayPeerInfo decode_relay_peer_info(const Message& message) {
  if (message.type != "peer") throw KikoError("expected peer");

  RelayPeerInfo peer;
  peer.peer_public =
      Endpoint{message.get("peer_public_host"), message_port_or(message, "peer_public_port", 0, true)};
  peer.peer_listen =
      Endpoint{message.get("peer_listen_host"), message_port_or(message, "peer_listen_port", 0, true)};
  peer.peer_local_candidates = split_csv(message.get("peer_local_candidates"));
  peer.peer_no_direct = message.get("peer_no_direct") == "1";
  peer.self_public =
      Endpoint{message.get("your_public_host"), message_port_or(message, "your_public_port", 0, true)};
  peer.punch_token = message.get("punch_token");
  peer.route_commit_v2 = message.get("route_commit") == "v2";
  peer.file_count = message.get_u64("file_count", 0);
  peer.total_size = message.get_u64("total_size", 0);
  peer.conn_count = message.get_u64("conn_count", 1);
  validate_connection_count(peer.conn_count);
  return peer;
}

}  // namespace kiko
