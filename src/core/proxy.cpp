#include "core/proxy.hpp"

#include "core/socket.hpp"

#include <asio/ip/address.hpp>

#include <array>
#include <sstream>
#include <string_view>
#include <vector>

namespace kiko {
namespace {

bool recv_line(TcpSocket& socket, std::string& line, std::chrono::milliseconds timeout) {
  line.clear();
  char ch = 0;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0 || !socket.recv_exact_timeout(&ch, 1, remaining)) return false;
    line.push_back(ch);
    if (line.size() >= 2 && line[line.size() - 2] == '\r' && line.back() == '\n') {
      while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
      return true;
    }
  }
  return false;
}

bool http_connect_ok(const std::string& status_line) {
  std::istringstream in(status_line);
  std::string version;
  int status = 0;
  if (!(in >> version >> status)) return false;
  return status == 200;
}

void http_connect(TcpSocket& socket, const Endpoint& target, std::chrono::milliseconds timeout) {
  const auto hostport = target.host + ":" + std::to_string(target.port);
  const std::string req = "CONNECT " + hostport + " HTTP/1.1\r\nHost: " + hostport + "\r\nProxy-Connection: keep-alive\r\n\r\n";
  socket.send_all(req.data(), req.size());

  std::string line;
  if (!recv_line(socket, line, timeout)) throw KikoError("proxy: no response");
  if (!http_connect_ok(line)) throw KikoError("proxy CONNECT failed: " + line);
  while (recv_line(socket, line, timeout)) {
    if (line.empty()) break;
  }
}

void socks5_connect(TcpSocket& socket, const Endpoint& target, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  auto recv_or_throw = [&](void* data, std::size_t size, const char* error) {
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0 || !socket.recv_exact_timeout(data, size, remaining)) throw KikoError(error);
  };

  const std::array<std::uint8_t, 3> greeting{0x05, 0x01, 0x00};
  socket.send_all(greeting.data(), greeting.size());
  std::array<std::uint8_t, 2> method{};
  recv_or_throw(method.data(), method.size(), "proxy: SOCKS greeting failed");
  if (method[0] != 0x05 || method[1] != 0x00) throw KikoError("proxy: unsupported SOCKS auth");

  std::vector<std::uint8_t> req;
  req.push_back(0x05);
  req.push_back(0x01);
  req.push_back(0x00);

  asio::error_code ec;
  auto addr = asio::ip::make_address(target.host, ec);
  if (!ec && addr.is_v4()) {
    req.push_back(0x01);
    auto bytes = addr.to_v4().to_bytes();
    req.insert(req.end(), bytes.begin(), bytes.end());
  } else if (!ec && addr.is_v6()) {
    req.push_back(0x04);
    auto bytes = addr.to_v6().to_bytes();
    req.insert(req.end(), bytes.begin(), bytes.end());
  } else {
    if (target.host.size() > 255) throw KikoError("proxy: hostname too long");
    req.push_back(0x03);
    req.push_back(static_cast<std::uint8_t>(target.host.size()));
    req.insert(req.end(), target.host.begin(), target.host.end());
  }
  req.push_back(static_cast<std::uint8_t>((target.port >> 8) & 0xff));
  req.push_back(static_cast<std::uint8_t>(target.port & 0xff));
  socket.send_all(req.data(), req.size());

  std::array<std::uint8_t, 4> head{};
  recv_or_throw(head.data(), head.size(), "proxy: SOCKS connect failed");
  if (head[1] != 0x00) throw KikoError("proxy: SOCKS connect rejected");

  std::size_t skip = 0;
  switch (head[3]) {
    case 0x01:
      skip = 4;
      break;
    case 0x04:
      skip = 16;
      break;
    case 0x03: {
      std::uint8_t len = 0;
      recv_or_throw(&len, 1, "proxy: SOCKS domain length failed");
      skip = len;
      break;
    }
    default:
      throw KikoError("proxy: SOCKS bad address type");
  }
  skip += 2;
  std::vector<std::uint8_t> junk(skip);
  recv_or_throw(junk.data(), junk.size(), "proxy: SOCKS bind read failed");
}

}  // namespace

std::optional<ProxyConfig> parse_proxy_url(const std::string& url) {
  if (url.empty()) return std::nullopt;
  auto scheme_end = url.find("://");
  if (scheme_end == std::string::npos) return std::nullopt;
  auto scheme = lowercase_ascii(url.substr(0, scheme_end));
  auto rest = url.substr(scheme_end + 3);
  ProxyConfig cfg;
  if (scheme == "http" || scheme == "https") {
    cfg.type = ProxyType::Http;
  } else if (scheme == "socks5" || scheme == "socks") {
    cfg.type = ProxyType::Socks5;
  } else {
    return std::nullopt;
  }
  cfg.endpoint = parse_endpoint(rest, scheme == "https" ? 443 : 8080);
  return cfg;
}

void proxy_connect(TcpSocket& proxy_socket, const Endpoint& target, const ProxyConfig& proxy,
                   std::chrono::milliseconds timeout) {
  if (proxy.type == ProxyType::Http) {
    http_connect(proxy_socket, target, timeout);
  } else {
    socks5_connect(proxy_socket, target, timeout);
  }
}

}  // namespace kiko
