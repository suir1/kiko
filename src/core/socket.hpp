#pragma once

#include "core/common.hpp"
#include "core/proxy.hpp"

#include <asio/awaitable.hpp>
#include <asio/ip/tcp.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace kiko {

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
  TcpSocket(TcpSocket&&) noexcept = default;
  TcpSocket& operator=(TcpSocket&&) noexcept = default;
  ~TcpSocket();

  [[nodiscard]] bool valid() const;
  [[nodiscard]] int fd() const;
  [[nodiscard]] asio::ip::tcp::socket& asio_socket();
  [[nodiscard]] const asio::ip::tcp::socket& asio_socket() const;
  [[nodiscard]] std::weak_ptr<asio::ip::tcp::socket> weak_handle() const;

  void close();
  void set_no_delay(bool enabled);
  void set_reuse_addr(bool enabled);
  void set_ipv6_only(bool enabled);
  void set_blocking(bool blocking);
  void send_all(const void* data, std::size_t size);
  [[nodiscard]] bool recv_exact(void* data, std::size_t size);
  [[nodiscard]] bool recv_exact_timeout(void* data, std::size_t size, std::chrono::milliseconds timeout,
                                        const std::atomic_bool* cancel = nullptr);

  asio::awaitable<void> async_send_all(const void* data, std::size_t size);
  [[nodiscard]] asio::awaitable<bool> async_recv_exact(void* data, std::size_t size);

  [[nodiscard]] Endpoint local_endpoint() const;
  [[nodiscard]] Endpoint peer_endpoint() const;

 private:
  std::shared_ptr<asio::ip::tcp::socket> socket_;
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
  [[nodiscard]] bool valid() const;
  [[nodiscard]] std::shared_ptr<asio::ip::tcp::acceptor> acceptor() const { return acceptor_; }

 private:
  std::shared_ptr<asio::ip::tcp::acceptor> acceptor_;
};

[[nodiscard]] TcpSocket connect_tcp(const Endpoint& endpoint, std::chrono::milliseconds timeout,
                                    const std::optional<ProxyConfig>& proxy = std::nullopt);
[[nodiscard]] TcpSocket connect_tcp(const Endpoint& endpoint, std::chrono::milliseconds timeout,
                                    const ConnectOptions& options, const std::atomic_bool* cancel = nullptr);
[[nodiscard]] std::vector<std::string> local_interface_addresses();
// LAN/private candidates for punch and hello; excludes typical VPN tunnel interfaces.
[[nodiscard]] std::vector<std::string> local_lan_candidate_addresses();

}  // namespace kiko
