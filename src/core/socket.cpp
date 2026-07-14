#include "core/socket.hpp"

#include "core/io.hpp"
#include "core/proxy.hpp"
#include "platform/platform.hpp"

#include <asio/connect.hpp>
#include <asio/ip/v6_only.hpp>
#include <asio/read.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include <algorithm>
#include <mutex>
#include <vector>

namespace kiko {
namespace {

using NativeSocketHandle = asio::ip::tcp::socket::native_handle_type;

bool operation_cancelled(const std::atomic_bool* cancel) { return cancel && cancel->load(); }

Endpoint from_asio_endpoint(const asio::ip::tcp::endpoint& endpoint) {
  return Endpoint{endpoint.address().to_string(), endpoint.port()};
}

NativeSocketHandle invalid_native_socket() {
#ifdef _WIN32
  return INVALID_SOCKET;
#else
  return -1;
#endif
}

bool native_socket_valid(NativeSocketHandle handle) {
#ifdef _WIN32
  return handle != INVALID_SOCKET;
#else
  return handle >= 0;
#endif
}

void interrupt_native_socket(NativeSocketHandle handle) {
  if (!native_socket_valid(handle)) return;
#ifdef _WIN32
  (void)::shutdown(handle, SD_BOTH);
#else
  (void)::shutdown(handle, SHUT_RDWR);
#endif
}

std::int64_t recv_native(NativeSocketHandle handle, void* data, std::size_t size) {
#ifdef _WIN32
  return ::recv(handle, static_cast<char*>(data), static_cast<int>(size), 0);
#else
  return ::recv(handle, data, size, 0);
#endif
}

bool native_recv_closed(int error) {
#ifdef _WIN32
  return error == WSAECONNRESET || error == WSAENOTCONN || error == WSAESHUTDOWN;
#else
  return error == ECONNRESET || error == ENOTCONN;
#endif
}

std::vector<asio::ip::tcp::endpoint> resolve_endpoints(const Endpoint& endpoint, bool passive) {
  std::vector<asio::ip::tcp::endpoint> out;

  if (passive && endpoint.host.empty()) {
    for (auto protocol : {asio::ip::tcp::v6(), asio::ip::tcp::v4()}) {
      out.emplace_back(protocol, endpoint.port);
    }
    return out;
  }

  asio::error_code ec;
  if (!endpoint.host.empty()) {
    auto address = asio::ip::make_address(endpoint.host, ec);
    if (!ec) {
      out.emplace_back(address, endpoint.port);
      return out;
    }
  }

  asio::ip::tcp::resolver resolver(io_context());
  const char* host = endpoint.host.empty() ? nullptr : endpoint.host.c_str();
  auto port = std::to_string(endpoint.port);
  ec.clear();
  auto results = resolver.resolve(host, port, ec);
  if (ec) throw KikoError("resolve failed: " + ec.message());
  for (const auto& entry : results) out.push_back(entry.endpoint());

  std::stable_sort(out.begin(), out.end(), [](const asio::ip::tcp::endpoint& a, const asio::ip::tcp::endpoint& b) {
    return a.protocol() == asio::ip::tcp::v6() && b.protocol() != asio::ip::tcp::v6();
  });
  return out;
}

void configure_socket(asio::ip::tcp::socket& socket) {
  asio::error_code ec;
  socket.set_option(asio::ip::tcp::no_delay(true), ec);
}

void set_reuse_port_native(int fd) {
#if !defined(_WIN32) && defined(SO_REUSEPORT)
  int enabled = 1;
  (void)setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled));
#else
  (void)fd;
#endif
}

void set_reuse_options(asio::ip::tcp::socket& socket) {
  asio::error_code ec;
  socket.set_option(asio::ip::tcp::acceptor::reuse_address(true), ec);
  set_reuse_port_native(static_cast<int>(socket.native_handle()));
}

void set_reuse_options(asio::ip::tcp::acceptor& acceptor) {
  asio::error_code ec;
  acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true), ec);
  set_reuse_port_native(static_cast<int>(acceptor.native_handle()));
}

bool bind_socket_to_interface(asio::ip::tcp::socket& socket, const asio::ip::tcp::endpoint& candidate,
                              const std::string& interface_name, std::string& error) {
  if (interface_name.empty() || candidate.address().is_loopback()) return true;

#ifdef _WIN32
  error = "interface binding is unsupported on Windows";
  return false;
#else
  const unsigned int if_index = if_nametoindex(interface_name.c_str());
  if (if_index == 0) {
    error = "unknown interface " + interface_name;
    return false;
  }
  const int fd = static_cast<int>(socket.native_handle());
#ifdef __APPLE__
  const int level = candidate.address().is_v6() ? IPPROTO_IPV6 : IPPROTO_IP;
  const int option = candidate.address().is_v6() ? IPV6_BOUND_IF : IP_BOUND_IF;
  if (setsockopt(fd, level, option, &if_index, sizeof(if_index)) != 0) {
    error = "bind interface " + interface_name + " failed: " + net_error_string(net_last_error());
    return false;
  }
  return true;
#elif defined(SO_BINDTODEVICE)
  if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, interface_name.c_str(), interface_name.size() + 1) != 0) {
    error = "bind interface " + interface_name + " failed: " + net_error_string(net_last_error());
    return false;
  }
  return true;
#else
  (void)fd;
  error = "interface binding is unsupported on this platform";
  return false;
#endif
#endif
}

std::optional<asio::ip::tcp::endpoint> local_bind_endpoint_for(const asio::ip::tcp::endpoint& candidate,
                                                               const std::optional<Endpoint>& local_bind) {
  if (!local_bind) return std::nullopt;
  if (local_bind->host.empty() || local_bind->host == "0.0.0.0" || local_bind->host == "::" ||
      local_bind->host == "[::]") {
    return asio::ip::tcp::endpoint(candidate.protocol(), local_bind->port);
  }

  asio::error_code ec;
  auto address = asio::ip::make_address(local_bind->host, ec);
  if (!ec) {
    if (address.is_v4() != candidate.address().is_v4()) return std::nullopt;
    return asio::ip::tcp::endpoint(address, local_bind->port);
  }

  try {
    auto endpoints = resolve_endpoints(*local_bind, /*passive=*/true);
    for (const auto& endpoint : endpoints) {
      if (endpoint.protocol() == candidate.protocol()) return endpoint;
    }
  } catch (const KikoError&) {
  }
  return std::nullopt;
}

bool bind_socket_to_local_endpoint(asio::ip::tcp::socket& socket, const asio::ip::tcp::endpoint& candidate,
                                   const std::optional<Endpoint>& local_bind, std::string& error) {
  auto bind_endpoint = local_bind_endpoint_for(candidate, local_bind);
  if (!bind_endpoint) return !local_bind.has_value();

  set_reuse_options(socket);
  asio::error_code ec;
  socket.bind(*bind_endpoint, ec);
  if (ec) {
    error = "local bind " + from_asio_endpoint(*bind_endpoint).to_string() + " failed: " + ec.message();
    return false;
  }
  return true;
}

}  // namespace

struct SocketInterruptHandle::State {
  std::mutex mutex;
  NativeSocketHandle native_handle = invalid_native_socket();
};

SocketInterruptHandle::SocketInterruptHandle(std::weak_ptr<State> state) : state_(std::move(state)) {}

void SocketInterruptHandle::interrupt() const {
  auto state = state_.lock();
  if (!state) return;
  std::lock_guard<std::mutex> lock(state->mutex);
  interrupt_native_socket(state->native_handle);
}

bool SocketInterruptHandle::expired() const { return state_.expired(); }

TcpSocket::TcpSocket()
    : socket_(std::make_shared<asio::ip::tcp::socket>(io_context())),
      interrupt_state_(std::make_shared<SocketInterruptHandle::State>()) {}

TcpSocket::TcpSocket(asio::ip::tcp::socket socket)
    : socket_(std::make_shared<asio::ip::tcp::socket>(std::move(socket))),
      interrupt_state_(std::make_shared<SocketInterruptHandle::State>()) {
  interrupt_state_->native_handle = socket_->native_handle();
}

TcpSocket::TcpSocket(TcpSocket&& other) noexcept
    : socket_(std::move(other.socket_)), interrupt_state_(std::move(other.interrupt_state_)) {}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
  if (this == &other) return *this;
  close();
  socket_ = std::move(other.socket_);
  interrupt_state_ = std::move(other.interrupt_state_);
  return *this;
}

TcpSocket::~TcpSocket() { close(); }

bool TcpSocket::valid() const { return socket_ && socket_->is_open(); }

int TcpSocket::fd() const {
  if (!valid()) return -1;
  return static_cast<int>(socket_->native_handle());
}

asio::ip::tcp::socket& TcpSocket::asio_socket() { return *socket_; }
const asio::ip::tcp::socket& TcpSocket::asio_socket() const { return *socket_; }
SocketInterruptHandle TcpSocket::interrupt_handle() const { return SocketInterruptHandle(interrupt_state_); }

void TcpSocket::interrupt() const { interrupt_handle().interrupt(); }

void TcpSocket::close() {
  if (!socket_) return;
  std::unique_lock<std::mutex> interrupt_lock;
  if (interrupt_state_) {
    interrupt_lock = std::unique_lock<std::mutex>(interrupt_state_->mutex);
    interrupt_state_->native_handle = invalid_native_socket();
  }
  asio::error_code ec;
  if (socket_->is_open()) {
    socket_->shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    ec.clear();
  }
  socket_->close(ec);
}

void TcpSocket::set_no_delay(bool enabled) {
  asio::error_code ec;
  socket_->set_option(asio::ip::tcp::no_delay(enabled), ec);
  if (ec) throw KikoError("set TCP_NODELAY failed: " + ec.message());
}

void TcpSocket::set_reuse_addr(bool enabled) {
  asio::error_code ec;
  socket_->set_option(asio::ip::tcp::acceptor::reuse_address(enabled), ec);
  if (ec) throw KikoError("set SO_REUSEADDR failed: " + ec.message());
}

void TcpSocket::set_ipv6_only(bool enabled) {
  asio::error_code ec;
  socket_->set_option(asio::ip::v6_only(enabled), ec);
}

void TcpSocket::set_blocking(bool blocking) {
  asio::error_code ec;
  socket_->native_non_blocking(!blocking, ec);
  if (ec) throw KikoError("set blocking failed: " + ec.message());
}

void TcpSocket::send_all(const void* data, std::size_t size) {
  asio::write(*socket_, asio::buffer(data, size));
}

bool TcpSocket::recv_exact(void* data, std::size_t size) {
  asio::error_code ec;
  asio::read(*socket_, asio::buffer(data, size), asio::transfer_exactly(size), ec);
  if (ec == asio::error::eof) return false;
  if (ec) throw KikoError("recv failed: " + ec.message());
  return true;
}

bool TcpSocket::recv_exact_timeout(void* data, std::size_t size, std::chrono::milliseconds timeout,
                                   const std::atomic_bool* cancel) {
  if (size == 0) return true;
  if (operation_cancelled(cancel)) return false;

  NativeSocketHandle handle = invalid_native_socket();
  if (interrupt_state_) {
    std::lock_guard<std::mutex> lock(interrupt_state_->mutex);
    handle = interrupt_state_->native_handle;
  }
  if (!native_socket_valid(handle)) return false;

  auto* ptr = static_cast<std::uint8_t*>(data);
  std::size_t received = 0;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (received < size) {
    if (operation_cancelled(cancel)) return false;

    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) return false;
    const auto slice = cancel ? std::chrono::milliseconds(25) : std::chrono::milliseconds(50);
    const int poll_ms = static_cast<int>(std::min<std::int64_t>(remaining.count(), slice.count()));
    const int poll_result =
        net_poll(static_cast<int>(handle), /*want_read=*/true, /*want_write=*/false, poll_ms);
    if (poll_result == 0) continue;
    if (poll_result < 0) {
      if (net_last_error() == kErrIntr) continue;
      return false;
    }

    const auto n = recv_native(handle, ptr + received, size - received);
    if (n > 0) {
      received += static_cast<std::size_t>(n);
      continue;
    }
    if (n == 0) return false;

    const int error = net_last_error();
    if (error == kErrIntr || error == kErrWouldBlock) continue;
    if (native_recv_closed(error)) return false;
    throw KikoError("recv failed: " + net_error_string(error));
  }
  return true;
}

asio::awaitable<void> TcpSocket::async_send_all(const void* data, std::size_t size) {
  co_await asio::async_write(*socket_, asio::buffer(data, size), asio::use_awaitable);
}

asio::awaitable<bool> TcpSocket::async_recv_exact(void* data, std::size_t size) {
  std::size_t received = 0;
  auto* ptr = static_cast<std::uint8_t*>(data);
  while (received < size) {
    std::size_t n = co_await socket_->async_read_some(asio::buffer(ptr + received, size - received), asio::use_awaitable);
    if (n == 0) co_return false;
    received += n;
  }
  co_return true;
}

Endpoint TcpSocket::local_endpoint() const {
  asio::error_code ec;
  auto ep = socket_->local_endpoint(ec);
  if (ec) throw KikoError("local_endpoint failed: " + ec.message());
  return from_asio_endpoint(ep);
}

Endpoint TcpSocket::peer_endpoint() const {
  asio::error_code ec;
  auto ep = socket_->remote_endpoint(ec);
  if (ec) throw KikoError("peer_endpoint failed: " + ec.message());
  return from_asio_endpoint(ep);
}

TcpListener::TcpListener(asio::ip::tcp::acceptor acceptor) : acceptor_(std::make_shared<asio::ip::tcp::acceptor>(std::move(acceptor))) {}

TcpListener TcpListener::bind(const Endpoint& endpoint, int backlog) {
  auto candidates = resolve_endpoints(endpoint, /*passive=*/true);
  if (candidates.empty()) throw KikoError("no addresses to bind for " + endpoint.to_string());

  std::string last_error = "no candidate addresses";
  for (const auto& candidate : candidates) {
    asio::error_code ec;
    asio::ip::tcp::acceptor acceptor(io_context());
    acceptor.open(candidate.protocol(), ec);
    if (ec) {
      last_error = ec.message();
      continue;
    }
    set_reuse_options(acceptor);
    if (candidate.protocol() == asio::ip::tcp::v6()) {
      acceptor.set_option(asio::ip::v6_only(false), ec);
    }
    acceptor.bind(candidate, ec);
    if (ec) {
      last_error = ec.message();
      continue;
    }
    acceptor.listen(backlog, ec);
    if (ec) {
      last_error = ec.message();
      continue;
    }
    return TcpListener(std::move(acceptor));
  }
  throw KikoError(last_error);
}

TcpSocket TcpListener::accept(std::chrono::milliseconds timeout) {
  if (!acceptor_) return TcpSocket();
  asio::error_code ec;
  acceptor_->non_blocking(true, ec);

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    asio::ip::tcp::socket socket(io_context());
    acceptor_->accept(socket, ec);
    if (!ec) {
      configure_socket(socket);
      socket.native_non_blocking(false, ec);
      return TcpSocket(std::move(socket));
    }
    if (ec != asio::error::would_block && ec != asio::error::try_again) {
      throw KikoError("accept failed: " + ec.message());
    }
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) break;
    if (net_poll(static_cast<int>(acceptor_->native_handle()), /*want_read=*/true, /*want_write=*/false,
                 static_cast<int>(remaining.count())) <= 0) {
      continue;
    }
  }
  return TcpSocket();
}

Endpoint TcpListener::local_endpoint() const {
  if (!acceptor_) throw KikoError("listener not bound");
  asio::error_code ec;
  auto ep = acceptor_->local_endpoint(ec);
  if (ec) throw KikoError("local_endpoint failed: " + ec.message());
  return from_asio_endpoint(ep);
}

bool TcpListener::valid() const { return acceptor_ && acceptor_->is_open(); }

TcpSocket connect_tcp(const Endpoint& endpoint, std::chrono::milliseconds timeout,
                      const std::optional<ProxyConfig>& proxy) {
  ConnectOptions options;
  options.proxy = proxy;
  return connect_tcp(endpoint, timeout, options, nullptr);
}

TcpSocket connect_tcp(const Endpoint& endpoint, std::chrono::milliseconds timeout, const ConnectOptions& options,
                      const std::atomic_bool* cancel) {
  if (operation_cancelled(cancel)) return TcpSocket();

  if (options.proxy) {
    ConnectOptions tunnel_options;
    tunnel_options.bind_interface = options.bind_interface;
    tunnel_options.local_bind = options.local_bind;
    auto tunnel = connect_tcp(options.proxy->endpoint, timeout, tunnel_options, cancel);
    if (!tunnel.valid()) return tunnel;
    if (operation_cancelled(cancel)) {
      tunnel.close();
      return TcpSocket();
    }
    try {
      proxy_connect(tunnel, endpoint, *options.proxy, timeout);
    } catch (...) {
      tunnel.close();
      return TcpSocket();
    }
    if (operation_cancelled(cancel)) {
      tunnel.close();
      return TcpSocket();
    }
    return tunnel;
  }

  std::vector<asio::ip::tcp::endpoint> candidates;
  try {
    candidates = resolve_endpoints(endpoint, /*passive=*/false);
  } catch (const KikoError&) {
    return TcpSocket();
  }

  for (const auto& candidate : candidates) {
    if (operation_cancelled(cancel)) return TcpSocket();
    asio::ip::tcp::socket socket(io_context());
    asio::error_code ec;
    socket.open(candidate.protocol(), ec);
    if (ec) continue;
    configure_socket(socket);
    std::string bind_error;
    if (!bind_socket_to_interface(socket, candidate, options.bind_interface, bind_error)) {
      continue;
    }
    if (!bind_socket_to_local_endpoint(socket, candidate, options.local_bind, bind_error)) {
      continue;
    }
    socket.native_non_blocking(true, ec);
    if (ec) continue;

#ifdef _WIN32
    const int rc = ::connect(static_cast<SOCKET>(socket.native_handle()), candidate.data(),
                             static_cast<int>(candidate.size()));
#else
    const int rc = ::connect(static_cast<int>(socket.native_handle()), candidate.data(),
                             static_cast<socklen_t>(candidate.size()));
#endif
    if (rc == 0) {
      if (operation_cancelled(cancel)) return TcpSocket();
      socket.native_non_blocking(false, ec);
      if (ec) continue;
      return TcpSocket(std::move(socket));
    }

    const int connect_error = net_last_error();
    if (connect_error != kErrInProgress && connect_error != kErrWouldBlock) continue;

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!operation_cancelled(cancel) && std::chrono::steady_clock::now() < deadline) {
      auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      if (remaining.count() <= 0) break;
      const auto slice = cancel ? std::chrono::milliseconds(25) : remaining;
      if (net_poll(static_cast<int>(socket.native_handle()), /*want_read=*/false, /*want_write=*/true,
                   static_cast<int>(std::min(remaining, slice).count())) <= 0) {
        continue;
      }
      if (operation_cancelled(cancel)) break;
      int error = 0;
      socklen_t error_len = sizeof(error);
      if (getsockopt(socket.native_handle(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &error_len) != 0 || error != 0) {
        break;
      }
      socket.native_non_blocking(false, ec);
      if (ec) break;
      if (operation_cancelled(cancel)) break;
      return TcpSocket(std::move(socket));
    }
  }
  return TcpSocket();
}

}  // namespace kiko
