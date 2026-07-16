#pragma once

#include "core/common.hpp"
#include "core/socket.hpp"

#include <atomic>
#include <chrono>
#include <map>
#include <optional>
#include <string>

namespace kiko {

struct Message {
  std::string type;
  std::map<std::string, std::string> fields;

  [[nodiscard]] std::string get(const std::string& key, const std::string& fallback = "") const;
  [[nodiscard]] std::uint64_t get_u64(const std::string& key, std::uint64_t fallback = 0) const;
};

[[nodiscard]] std::optional<std::uint16_t> message_port_field(const Message& message, const std::string& key,
                                                              bool allow_zero = false);
[[nodiscard]] std::uint16_t message_port_or(const Message& message, const std::string& key, std::uint16_t fallback,
                                            bool allow_zero = false);

void send_frame(TcpSocket& socket, const Bytes& payload);
[[nodiscard]] std::optional<Bytes> recv_frame(TcpSocket& socket);
[[nodiscard]] std::optional<Bytes> recv_frame_timeout(TcpSocket& socket, std::chrono::milliseconds timeout,
                                                      const std::atomic_bool* cancel = nullptr);

[[nodiscard]] std::string encode_message(const Message& message);
[[nodiscard]] Message decode_message(const std::string& text);
void send_message(TcpSocket& socket, const Message& message);
[[nodiscard]] std::optional<Message> recv_message(TcpSocket& socket);
[[nodiscard]] std::optional<Message> recv_message_timeout(TcpSocket& socket, std::chrono::milliseconds timeout,
                                                          const std::atomic_bool* cancel = nullptr);

}  // namespace kiko
