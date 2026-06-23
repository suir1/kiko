#pragma once

#include "common.hpp"

#include <chrono>
#include <optional>
#include <string>

namespace kiko {

class TcpSocket;

enum class ProxyType { Http, Socks5 };

struct ProxyConfig {
  ProxyType type = ProxyType::Http;
  Endpoint endpoint;
};

[[nodiscard]] std::optional<ProxyConfig> parse_proxy_url(const std::string& url);

// Connects to target through an established proxy tunnel (HTTP CONNECT or SOCKS5).
void proxy_connect(TcpSocket& proxy_socket, const Endpoint& target, const ProxyConfig& proxy,
                   std::chrono::milliseconds timeout);

}  // namespace kiko
