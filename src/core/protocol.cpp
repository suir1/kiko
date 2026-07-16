#include "core/protocol.hpp"

#include "platform/platform.hpp"

#include <nlohmann/json.hpp>

namespace kiko {
namespace {

constexpr char kMagic[4] = {'k', 'i', 'k', 'o'};

void verify_magic(const std::uint8_t* header) {
  for (int i = 0; i < 4; ++i) {
    if (header[i] != static_cast<std::uint8_t>(kMagic[i])) {
      throw KikoError("invalid frame magic");
    }
  }
}

}  // namespace

std::string Message::get(const std::string& key, const std::string& fallback) const {
  auto it = fields.find(key);
  return it == fields.end() ? fallback : it->second;
}

std::uint64_t Message::get_u64(const std::string& key, std::uint64_t fallback) const {
  auto value = get(key);
  if (value.empty()) return fallback;
  auto number = parse_u64_strict(value);
  if (!number) throw KikoError("control message field is not an unsigned integer: " + key);
  return *number;
}

std::optional<std::uint16_t> message_port_field(const Message& message, const std::string& key, bool allow_zero) {
  const auto value = message.get(key);
  if (value.empty()) return std::nullopt;
  const auto port = message.get_u64(key, 0);
  if (port > 65535 || (!allow_zero && port == 0)) throw KikoError("invalid port field: " + key);
  return static_cast<std::uint16_t>(port);
}

std::uint16_t message_port_or(const Message& message, const std::string& key, std::uint16_t fallback, bool allow_zero) {
  auto port = message_port_field(message, key, allow_zero);
  return port.value_or(fallback);
}

void send_frame(TcpSocket& socket, const Bytes& payload) {
  if (payload.size() > 64ull * 1024ull * 1024ull) throw KikoError("frame too large");
  socket.send_all(kMagic, sizeof(kMagic));
  std::uint32_t len = htonl(static_cast<std::uint32_t>(payload.size()));
  socket.send_all(&len, sizeof(len));
  if (!payload.empty()) socket.send_all(payload.data(), payload.size());
}

std::optional<Bytes> recv_frame(TcpSocket& socket) {
  std::uint8_t magic[4]{};
  if (!socket.recv_exact(magic, sizeof(magic))) return std::nullopt;
  verify_magic(magic);

  std::uint32_t be_len = 0;
  if (!socket.recv_exact(&be_len, sizeof(be_len))) return std::nullopt;
  auto len = ntohl(be_len);
  if (len > 64ull * 1024ull * 1024ull) throw KikoError("received frame too large");
  Bytes payload(len);
  if (len > 0 && !socket.recv_exact(payload.data(), payload.size())) return std::nullopt;
  return payload;
}

std::optional<Bytes> recv_frame_timeout(TcpSocket& socket, std::chrono::milliseconds timeout,
                                        const std::atomic_bool* cancel) {
  std::uint8_t magic[4]{};
  if (!socket.recv_exact_timeout(magic, sizeof(magic), timeout, cancel)) return std::nullopt;
  verify_magic(magic);

  std::uint32_t be_len = 0;
  if (!socket.recv_exact_timeout(&be_len, sizeof(be_len), timeout, cancel)) return std::nullopt;
  auto len = ntohl(be_len);
  if (len > 64ull * 1024ull * 1024ull) throw KikoError("received frame too large");
  Bytes payload(len);
  if (len > 0 && !socket.recv_exact_timeout(payload.data(), payload.size(), timeout, cancel)) return std::nullopt;
  return payload;
}

std::string encode_message(const Message& message) {
  nlohmann::json object = nlohmann::json::object();
  object["type"] = message.type;
  for (const auto& [key, value] : message.fields) {
    object[key] = value;
  }
  return object.dump();
}

Message decode_message(const std::string& text) {
  nlohmann::json object;
  try {
    object = nlohmann::json::parse(text);
  } catch (const nlohmann::json::exception& error) {
    throw KikoError(std::string("invalid control message json: ") + error.what());
  }
  if (!object.is_object()) throw KikoError("control message must be a json object");
  if (!object.contains("type") || !object["type"].is_string()) throw KikoError("control message missing type");

  Message message;
  message.type = object["type"].get<std::string>();
  for (auto it = object.begin(); it != object.end(); ++it) {
    if (it.key() == "type") continue;
    if (!it.value().is_string()) throw KikoError("control message field must be a string: " + it.key());
    message.fields[it.key()] = it.value().get<std::string>();
  }
  return message;
}

void send_message(TcpSocket& socket, const Message& message) {
  const auto text = encode_message(message);
  send_frame(socket, Bytes(text.begin(), text.end()));
}

std::optional<Message> recv_message(TcpSocket& socket) {
  auto payload = recv_frame(socket);
  if (!payload) return std::nullopt;
  return decode_message(std::string(payload->begin(), payload->end()));
}

std::optional<Message> recv_message_timeout(TcpSocket& socket, std::chrono::milliseconds timeout,
                                            const std::atomic_bool* cancel) {
  auto payload = recv_frame_timeout(socket, timeout, cancel);
  if (!payload) return std::nullopt;
  return decode_message(std::string(payload->begin(), payload->end()));
}

}  // namespace kiko
