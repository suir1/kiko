#include "socket.hpp"

#include "io.hpp"
#include "platform.hpp"
#include "proxy.hpp"

#include <asio/connect.hpp>
#include <asio/ip/v6_only.hpp>
#include <asio/read.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include <algorithm>
#include <vector>

namespace kiko {
namespace {

Endpoint from_asio_endpoint(const asio::ip::tcp::endpoint& endpoint) {
  return Endpoint{endpoint.address().to_string(), endpoint.port()};
}

std::vector<asio::ip::tcp::endpoint> resolve_endpoints(const Endpoint& endpoint, bool passive) {
  asio::ip::tcp::resolver resolver(io_context());
  asio::error_code ec;
  std::vector<asio::ip::tcp::endpoint> out;

  if (passive && endpoint.host.empty()) {
    for (auto protocol : {asio::ip::tcp::v6(), asio::ip::tcp::v4()}) {
      out.emplace_back(protocol, endpoint.port);
    }
    return out;
  }

  const char* host = endpoint.host.empty() ? nullptr : endpoint.host.c_str();
  auto port = std::to_string(endpoint.port);
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

void add_unique(std::vector<std::string>& out, const std::string& text) {
  if (text.empty()) return;
  if (std::find(out.begin(), out.end(), text) == out.end()) out.push_back(text);
}

}  // namespace

TcpSocket::TcpSocket() : socket_(std::make_shared<asio::ip::tcp::socket>(io_context())) {}

TcpSocket::TcpSocket(asio::ip::tcp::socket socket) : socket_(std::make_shared<asio::ip::tcp::socket>(std::move(socket))) {}

TcpSocket::~TcpSocket() { close(); }

bool TcpSocket::valid() const { return socket_ && socket_->is_open(); }

int TcpSocket::fd() const {
  if (!valid()) return -1;
  return static_cast<int>(socket_->native_handle());
}

asio::ip::tcp::socket& TcpSocket::asio_socket() { return *socket_; }
const asio::ip::tcp::socket& TcpSocket::asio_socket() const { return *socket_; }

void TcpSocket::close() {
  if (!socket_) return;
  asio::error_code ec;
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

bool TcpSocket::recv_exact_timeout(void* data, std::size_t size, std::chrono::milliseconds timeout) {
  if (size == 0) return true;
  if (!valid()) return false;

  asio::error_code ec;
  socket_->non_blocking(true, ec);
  if (ec) throw KikoError("set nonblocking failed: " + ec.message());
  struct BlockingRestore {
    asio::ip::tcp::socket& socket;
    ~BlockingRestore() {
      asio::error_code ignored;
      socket.non_blocking(false, ignored);
    }
  } restore{*socket_};

  auto* ptr = static_cast<std::uint8_t*>(data);
  std::size_t received = 0;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (received < size) {
    std::size_t n = socket_->read_some(asio::buffer(ptr + received, size - received), ec);
    if (!ec) {
      if (n == 0) return false;
      received += n;
      continue;
    }
    if (ec == asio::error::eof || ec == asio::error::connection_reset) return false;
    if (ec != asio::error::would_block && ec != asio::error::try_again) {
      throw KikoError("recv failed: " + ec.message());
    }

    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) return false;
    const int poll_ms = static_cast<int>(std::min<std::int64_t>(remaining.count(), 50));
    if (net_poll(fd(), /*want_read=*/true, /*want_write=*/false, poll_ms) < 0) return false;
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
    acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true), ec);
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
  return connect_tcp(endpoint, timeout, options);
}

TcpSocket connect_tcp(const Endpoint& endpoint, std::chrono::milliseconds timeout, const ConnectOptions& options) {
  if (options.proxy) {
    ConnectOptions tunnel_options;
    tunnel_options.bind_interface = options.bind_interface;
    auto tunnel = connect_tcp(options.proxy->endpoint, timeout, tunnel_options);
    if (!tunnel.valid()) return tunnel;
    try {
      proxy_connect(tunnel, endpoint, *options.proxy, timeout);
    } catch (...) {
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
    asio::ip::tcp::socket socket(io_context());
    asio::error_code ec;
    socket.open(candidate.protocol(), ec);
    if (ec) continue;
    configure_socket(socket);
    std::string bind_error;
    if (!bind_socket_to_interface(socket, candidate, options.bind_interface, bind_error)) {
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
      socket.native_non_blocking(false, ec);
      if (ec) continue;
      return TcpSocket(std::move(socket));
    }

    const int connect_error = net_last_error();
    if (connect_error != kErrInProgress && connect_error != kErrWouldBlock) continue;

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      if (remaining.count() <= 0) break;
      if (net_poll(static_cast<int>(socket.native_handle()), /*want_read=*/false, /*want_write=*/true,
                   static_cast<int>(remaining.count())) <= 0) {
        continue;
      }
      int error = 0;
      socklen_t error_len = sizeof(error);
      if (getsockopt(socket.native_handle(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &error_len) != 0 || error != 0) {
        break;
      }
      socket.native_non_blocking(false, ec);
      if (ec) break;
      return TcpSocket(std::move(socket));
    }
  }
  return TcpSocket();
}

#ifdef _WIN32

std::vector<std::string> local_interface_addresses() {
  net_startup();
  std::vector<std::string> out;
  ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
  ULONG size = 15 * 1024;
  std::vector<unsigned char> buffer(size);
  auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
  if (GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, adapters, &size) == ERROR_BUFFER_OVERFLOW) {
    buffer.resize(size);
    adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
  }
  if (GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, adapters, &size) != NO_ERROR) return out;

  for (auto* adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
    if (adapter->OperStatus != IfOperStatusUp) continue;
    if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
    for (auto* ua = adapter->FirstUnicastAddress; ua != nullptr; ua = ua->Next) {
      char host[INET6_ADDRSTRLEN] = {};
      auto* sa = ua->Address.lpSockaddr;
      if (sa->sa_family == AF_INET) {
        auto* a = reinterpret_cast<sockaddr_in*>(sa);
        inet_ntop(AF_INET, &a->sin_addr, host, sizeof(host));
      } else if (sa->sa_family == AF_INET6) {
        auto* a = reinterpret_cast<sockaddr_in6*>(sa);
        if (IN6_IS_ADDR_LINKLOCAL(&a->sin6_addr)) continue;
        inet_ntop(AF_INET6, &a->sin6_addr, host, sizeof(host));
      } else {
        continue;
      }
      add_unique(out, host);
    }
  }
  return out;
}

std::vector<std::string> local_lan_candidate_addresses() {
  return local_interface_addresses();
}

#else

namespace {
bool is_vpn_interface_name(const char* name) {
  if (!name) return false;
  const std::string n(name);
  return n.rfind("tun", 0) == 0 || n.rfind("wg", 0) == 0 || n.rfind("utun", 0) == 0 || n.rfind("ppp", 0) == 0 ||
         n.rfind("ipsec", 0) == 0;
}
}  // namespace

std::vector<std::string> local_interface_addresses() {
  std::vector<std::string> out;
  ifaddrs* ifaddr = nullptr;
  if (getifaddrs(&ifaddr) != 0) return out;

  for (ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == nullptr) continue;
    if ((ifa->ifa_flags & IFF_UP) == 0) continue;
    if (ifa->ifa_flags & IFF_LOOPBACK) continue;

    char host[INET6_ADDRSTRLEN] = {};
    int family = ifa->ifa_addr->sa_family;
    if (family == AF_INET) {
      auto* a = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
      inet_ntop(AF_INET, &a->sin_addr, host, sizeof(host));
    } else if (family == AF_INET6) {
      auto* a = reinterpret_cast<sockaddr_in6*>(ifa->ifa_addr);
      if (IN6_IS_ADDR_LINKLOCAL(&a->sin6_addr)) continue;
      inet_ntop(AF_INET6, &a->sin6_addr, host, sizeof(host));
    } else {
      continue;
    }
    add_unique(out, host);
  }
  freeifaddrs(ifaddr);
  return out;
}

std::vector<std::string> local_lan_candidate_addresses() {
  std::vector<std::string> out;
  ifaddrs* ifaddr = nullptr;
  if (getifaddrs(&ifaddr) != 0) return out;

  for (ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == nullptr) continue;
    if ((ifa->ifa_flags & IFF_UP) == 0) continue;
    if (ifa->ifa_flags & IFF_LOOPBACK) continue;
    if (is_vpn_interface_name(ifa->ifa_name)) continue;

    char host[INET6_ADDRSTRLEN] = {};
    int family = ifa->ifa_addr->sa_family;
    if (family == AF_INET) {
      auto* a = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
      inet_ntop(AF_INET, &a->sin_addr, host, sizeof(host));
    } else if (family == AF_INET6) {
      auto* a = reinterpret_cast<sockaddr_in6*>(ifa->ifa_addr);
      if (IN6_IS_ADDR_LINKLOCAL(&a->sin6_addr)) continue;
      inet_ntop(AF_INET6, &a->sin6_addr, host, sizeof(host));
    } else {
      continue;
    }
    add_unique(out, host);
  }
  freeifaddrs(ifaddr);
  return out;
}

#endif

}  // namespace kiko
