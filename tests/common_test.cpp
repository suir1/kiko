#include "core/common.hpp"
#include "platform/platform.hpp"
#include "core/proxy.hpp"
#include "core/protocol.hpp"
#include "core/socket.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace {

bool throws_kiko_error(void (*fn)()) {
  try {
    fn();
  } catch (const kiko::KikoError&) {
    return true;
  }
  return false;
}

#ifndef _WIN32
bool fd_is_nonblocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  assert(flags >= 0);
  return (flags & O_NONBLOCK) != 0;
}
#endif

}  // namespace

int main() {
  using namespace kiko;

  {
    const auto ep = parse_endpoint("127.0.0.1", 9000);
    assert(ep.host == "127.0.0.1");
    assert(ep.port == 9000);
  }

  {
    const auto ep = parse_bind_endpoint("0.0.0.0:0");
    assert(ep.host == "0.0.0.0");
    assert(ep.port == 0);
  }

  {
    const auto ep = parse_bind_endpoint("[::]:0");
    assert(ep.host == "::");
    assert(ep.port == 0);
    assert(ep.to_string() == "[::]:0");
  }

  {
    const Endpoint ep{"127.0.0.1", 9000};
    assert(ep.to_string() == "127.0.0.1:9000");
  }

  {
    assert(ip_address_family("[2001:4860:4860::8888]") == IpAddressFamily::IPv6);
    assert(ip_address_scope("2001:4860:4860::8888") == IpAddressScope::Global);
    assert(std::string(ip_address_family_name(ip_address_family("2001:4860:4860::8888"))) == "ipv6");
    assert(std::string(ip_address_scope_name(ip_address_scope("2001:4860:4860::8888"))) == "global");
    assert(std::string(ip_address_scope_name(ip_address_scope("fd00::1"))) == "unique_local");
    assert(std::string(ip_address_scope_name(ip_address_scope("fe80::1"))) == "link_local");
    assert(is_global_ipv6_address("2001:4860:4860::8888"));
    assert(!is_global_ipv6_address("fd00::1"));
    assert(!is_global_ipv6_address("fe80::1"));
    assert(count_global_ipv6_addresses({"2001:4860:4860::8888", "fd00::1", "2001:4860:4860::8888"}) == 1);
  }

  {
    const auto ep = parse_bind_endpoint("127.0.0.1");
    assert(ep.host == "127.0.0.1");
    assert(ep.port == 0);
  }

  assert(throws_kiko_error([] { (void)parse_endpoint("127.0.0.1:0"); }));
  assert(throws_kiko_error([] { (void)parse_endpoint("[::]:0"); }));
  if (!throws_kiko_error([] { (void)parse_endpoint("127.0.0.1:abc"); })) {
    std::cerr << "FAIL: endpoint parser accepted non-numeric port\n";
    return 1;
  }
  if (!throws_kiko_error([] { (void)parse_endpoint("127.0.0.1:123abc"); })) {
    std::cerr << "FAIL: endpoint parser accepted trailing port text\n";
    return 1;
  }
  if (!throws_kiko_error([] { (void)parse_endpoint("127.0.0.1:+123"); })) {
    std::cerr << "FAIL: endpoint parser accepted signed port text\n";
    return 1;
  }
  if (!throws_kiko_error([] { (void)parse_endpoint("127.0.0.1: 123"); })) {
    std::cerr << "FAIL: endpoint parser accepted whitespace port text\n";
    return 1;
  }
  if (!throws_kiko_error([] {
        Message message{"test", {{"count", "12oops"}}};
        (void)message.get_u64("count", 0);
      })) {
    std::cerr << "FAIL: control message parser accepted trailing numeric text\n";
    return 1;
  }
  if (!throws_kiko_error([] {
        Message message{"test", {{"count", "+12"}}};
        (void)message.get_u64("count", 0);
      })) {
    std::cerr << "FAIL: control message parser accepted signed numeric text\n";
    return 1;
  }
  if (!throws_kiko_error([] {
        Message message{"test", {{"count", "-1"}}};
        (void)message.get_u64("count", 0);
      })) {
    std::cerr << "FAIL: control message parser accepted negative numeric text\n";
    return 1;
  }
  if (!throws_kiko_error([] {
        Message message{"test", {{"count", " 12"}}};
        (void)message.get_u64("count", 0);
      })) {
    std::cerr << "FAIL: control message parser accepted whitespace numeric text\n";
    return 1;
  }

  {
    auto listener = TcpListener::bind(Endpoint{"127.0.0.1", 0});
    const auto endpoint = listener.local_endpoint();
    bool writer_failed = false;
    std::thread writer([&] {
      auto socket = connect_tcp(endpoint, std::chrono::seconds(2));
      if (!socket.valid()) {
        writer_failed = true;
        return;
      }
      const std::uint8_t partial[2] = {0x01, 0x02};
      socket.send_all(partial, sizeof(partial));
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
    });

    auto accepted = listener.accept(std::chrono::seconds(2));
    if (!accepted.valid()) {
      writer.join();
      std::cerr << "FAIL: recv_exact_timeout test did not accept connection\n";
      return 1;
    }
    std::uint8_t full[4]{};
    const auto start = std::chrono::steady_clock::now();
    const bool complete = accepted.recv_exact_timeout(full, sizeof(full), std::chrono::milliseconds(50));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    writer.join();
    if (writer_failed) {
      std::cerr << "FAIL: recv_exact_timeout writer did not connect\n";
      return 1;
    }
    if (complete) {
      std::cerr << "FAIL: recv_exact_timeout returned complete for a partial frame\n";
      return 1;
    }
    if (elapsed >= std::chrono::seconds(1)) {
      std::cerr << "FAIL: recv_exact_timeout blocked too long\n";
      return 1;
    }
  }

  {
    auto listener = TcpListener::bind(Endpoint{"127.0.0.1", 0});
    const auto endpoint = listener.local_endpoint();
    bool accept_failed = false;
    std::thread accepter([&] {
      auto accepted = listener.accept(std::chrono::seconds(2));
      if (!accepted.valid()) {
        accept_failed = true;
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    });

    auto socket = connect_tcp(endpoint, std::chrono::seconds(2));
    if (!socket.valid()) {
      accepter.join();
      std::cerr << "FAIL: connect_tcp did not connect to loopback listener\n";
      return 1;
    }
#ifndef _WIN32
    if (fd_is_nonblocking(socket.fd())) {
      accepter.join();
      std::cerr << "FAIL: connect_tcp returned a non-blocking socket\n";
      return 1;
    }
#endif
    accepter.join();
    if (accept_failed) {
      std::cerr << "FAIL: listener did not accept connect_tcp connection\n";
      return 1;
    }
  }

  {
    std::uint16_t source_port = 0;
    {
      auto reservation = TcpListener::bind(Endpoint{"127.0.0.1", 0});
      source_port = reservation.local_endpoint().port;
    }

    auto listener = TcpListener::bind(Endpoint{"127.0.0.1", 0});
    const auto endpoint = listener.local_endpoint();
    Endpoint accepted_peer;
    bool accept_failed = false;
    std::thread accepter([&] {
      auto accepted = listener.accept(std::chrono::seconds(2));
      if (!accepted.valid()) {
        accept_failed = true;
        return;
      }
      accepted_peer = accepted.peer_endpoint();
    });

    ConnectOptions options;
    options.local_bind = Endpoint{"127.0.0.1", source_port};
    auto socket = connect_tcp(endpoint, std::chrono::seconds(2), options);
    if (!socket.valid()) {
      accepter.join();
      std::cerr << "FAIL: connect_tcp did not honor local bind endpoint\n";
      return 1;
    }
    const auto local = socket.local_endpoint();
    accepter.join();
    if (accept_failed) {
      std::cerr << "FAIL: listener did not accept local-bind connection\n";
      return 1;
    }
    if (local.port != source_port || accepted_peer.port != source_port) {
      std::cerr << "FAIL: local bind used source port " << local.port << " accepted peer port " << accepted_peer.port
                << " expected " << source_port << "\n";
      return 1;
    }
  }

  {
    auto proxy_listener = TcpListener::bind(Endpoint{"127.0.0.1", 0});
    const auto proxy_endpoint = proxy_listener.local_endpoint();
    bool proxy_accept_failed = false;
    std::thread silent_proxy([&] {
      auto accepted = proxy_listener.accept(std::chrono::seconds(2));
      if (!accepted.valid()) {
        proxy_accept_failed = true;
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    });

    ProxyConfig proxy{ProxyType::Http, proxy_endpoint};
    const auto start = std::chrono::steady_clock::now();
    auto socket = connect_tcp(Endpoint{"relay.example", 9000}, std::chrono::milliseconds(50), proxy);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    silent_proxy.join();
    if (proxy_accept_failed) {
      std::cerr << "FAIL: silent proxy did not accept test connection\n";
      return 1;
    }
    if (socket.valid()) {
      std::cerr << "FAIL: silent proxy unexpectedly established a tunnel\n";
      return 1;
    }
    if (elapsed >= std::chrono::milliseconds(150)) {
      std::cerr << "FAIL: proxy handshake ignored timeout after "
                << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << "ms\n";
      return 1;
    }
  }

  {
    auto proxy_listener = TcpListener::bind(Endpoint{"127.0.0.1", 0});
    const auto proxy_endpoint = proxy_listener.local_endpoint();
    bool proxy_accept_failed = false;
    std::thread fake_proxy([&] {
      auto accepted = proxy_listener.accept(std::chrono::seconds(2));
      if (!accepted.valid()) {
        proxy_accept_failed = true;
        return;
      }
      const std::string response = "HTTP/1.1 500 contains 200 in reason\r\n\r\n";
      accepted.send_all(response.data(), response.size());
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    });

    ProxyConfig proxy{ProxyType::Http, proxy_endpoint};
    auto socket = connect_tcp(Endpoint{"relay.example", 9000}, std::chrono::milliseconds(500), proxy);
    fake_proxy.join();
    if (proxy_accept_failed) {
      std::cerr << "FAIL: fake HTTP proxy did not accept test connection\n";
      return 1;
    }
    if (socket.valid()) {
      std::cerr << "FAIL: HTTP proxy accepted non-200 status because reason contained 200\n";
      return 1;
    }
  }

  {
    auto expect_valid_code = [](const std::string& code, bool required) {
      if (auto error = validate_pairing_code_format(code, required)) {
        std::cerr << "FAIL: expected valid pairing code '" << code << "': " << *error << "\n";
        return false;
      }
      return true;
    };
    auto expect_invalid_code = [](const std::string& code, bool required) {
      if (!validate_pairing_code_format(code, required)) {
        std::cerr << "FAIL: expected invalid pairing code '" << code << "'\n";
        return false;
      }
      return true;
    };

    if (!expect_valid_code("", false)) return 1;
    if (!expect_invalid_code("", true)) return 1;
    if (!expect_valid_code("abc234", false)) return 1;
    if (!expect_invalid_code("abc123", false)) return 1;
    if (!expect_invalid_code("abc12o", false)) return 1;
    if (!expect_valid_code("4827-stne-iris", false)) return 1;
    if (!expect_invalid_code("4827-stone-iris", false)) return 1;
    if (!expect_invalid_code("bad-chars!", false)) return 1;
    if (!expect_invalid_code("-abc", false)) return 1;
  }

  std::cout << "common_test ok\n";
  return 0;
}
