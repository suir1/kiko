#pragma once

#include "core/common.hpp"
#include "core/proxy.hpp"

#include <asio/ip/tcp.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace kiko {

class SocketInterruptHandle {
 public:
  SocketInterruptHandle() = default;

  void interrupt() const;
  [[nodiscard]] bool expired() const;

 private:
  struct State;

  explicit SocketInterruptHandle(std::weak_ptr<State> state);

  std::weak_ptr<State> state_;

  friend class TcpSocket;
};

struct ConnectOptions {
  std::optional<ProxyConfig> proxy;
  std::string bind_interface;
  std::optional<Endpoint> local_bind;
};

class TcpSocket {
 public:
  TcpSocket();
  explicit TcpSocket(asio::ip::tcp::socket socket);
  TcpSocket(const TcpSocket&) = delete;
  TcpSocket& operator=(const TcpSocket&) = delete;
  TcpSocket(TcpSocket&& other) noexcept;
  TcpSocket& operator=(TcpSocket&& other) noexcept;
  ~TcpSocket();

  [[nodiscard]] bool valid() const;
  [[nodiscard]] int fd() const;
  [[nodiscard]] asio::ip::tcp::socket& asio_socket();
  [[nodiscard]] SocketInterruptHandle interrupt_handle() const;

  void interrupt() const;
  void close();
  void set_blocking(bool blocking);
  void send_all(const void* data, std::size_t size);
  [[nodiscard]] bool recv_exact(void* data, std::size_t size);
  [[nodiscard]] bool recv_exact_timeout(void* data, std::size_t size, std::chrono::milliseconds timeout,
                                        const std::atomic_bool* cancel = nullptr);

  [[nodiscard]] Endpoint local_endpoint() const;
  [[nodiscard]] Endpoint peer_endpoint() const;

 private:
  std::shared_ptr<asio::ip::tcp::socket> socket_;
  std::shared_ptr<SocketInterruptHandle::State> interrupt_state_;
};

class TcpListener {
 public:
  TcpListener() = default;
  explicit TcpListener(asio::ip::tcp::acceptor acceptor);
  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;
  TcpListener(TcpListener&&) noexcept = default;
  TcpListener& operator=(TcpListener&&) noexcept = default;

  [[nodiscard]] static TcpListener bind(const Endpoint& endpoint, int backlog = 64);
  [[nodiscard]] TcpSocket accept(std::chrono::milliseconds timeout);
  [[nodiscard]] Endpoint local_endpoint() const;

 private:
  std::shared_ptr<asio::ip::tcp::acceptor> acceptor_;
};

[[nodiscard]] TcpSocket connect_tcp(const Endpoint& endpoint, std::chrono::milliseconds timeout,
                                    const std::optional<ProxyConfig>& proxy = std::nullopt);
[[nodiscard]] TcpSocket connect_tcp(const Endpoint& endpoint, std::chrono::milliseconds timeout,
                                    const ConnectOptions& options, const std::atomic_bool* cancel = nullptr);

}  // namespace kiko
