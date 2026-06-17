#include "network_probe.hpp"

#include "platform.hpp"
#include "socket.hpp"

#include <algorithm>
#include <map>

namespace kiko {
namespace {

constexpr const char* kStunPrimary = "stun.l.google.com";
constexpr const char* kStunSecondary = "stun1.l.google.com";
constexpr std::uint16_t kStunPort = 19302;

std::optional<Endpoint> stun_binding(const std::string& host, std::uint16_t port, std::chrono::milliseconds timeout) {
  net_startup();
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) return std::nullopt;

  int fd = -1;
  for (auto* ai = res; ai != nullptr; ai = ai->ai_next) {
    fd = static_cast<int>(socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
    if (fd < 0) continue;
    break;
  }
  if (fd < 0) {
    freeaddrinfo(res);
    return std::nullopt;
  }
  net_set_nonblocking(fd, true);

  std::uint8_t req[20]{};
  req[0] = 0x00;
  req[1] = 0x01;  // Binding Request
  req[4] = 0x21;
  req[5] = 0x12;
  req[6] = 0xA4;
  req[7] = 0x42;
  for (int i = 8; i < 20; ++i) req[i] = static_cast<std::uint8_t>(i);

  ssize_t sent = sendto(fd, req, sizeof(req), 0, res->ai_addr, static_cast<socklen_t>(res->ai_addrlen));
  if (sent != static_cast<ssize_t>(sizeof(req))) {
    net_close(fd);
    freeaddrinfo(res);
    return std::nullopt;
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::uint8_t buf[512]{};
  while (std::chrono::steady_clock::now() < deadline) {
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) break;
    if (net_poll(fd, true, false, static_cast<int>(std::min<std::int64_t>(remaining.count(), 50))) <= 0) continue;

    socklen_t from_len = 0;
    auto n = recvfrom(fd, buf, sizeof(buf), 0, nullptr, &from_len);
    if (n < 20) continue;

    const std::uint16_t msg_type = static_cast<std::uint16_t>((buf[0] << 8) | buf[1]);
    if ((msg_type & 0x0110) != 0x0100) continue;  // success response

    std::size_t off = 20;
    while (off + 4 <= static_cast<std::size_t>(n)) {
      const std::uint16_t attr_type = static_cast<std::uint16_t>((buf[off] << 8) | buf[off + 1]);
      const std::uint16_t attr_len = static_cast<std::uint16_t>((buf[off + 2] << 8) | buf[off + 3]);
      off += 4;
      if (off + attr_len > static_cast<std::size_t>(n)) break;
      if (attr_type == 0x0020 || attr_type == 0x0001) {  // XOR-MAPPED-ADDRESS or MAPPED-ADDRESS
        if (attr_len >= 8) {
          const std::uint8_t family = buf[off + 1];
          std::uint16_t xport = static_cast<std::uint16_t>((buf[off + 2] << 8) | buf[off + 3]);
          if (attr_type == 0x0020) xport ^= 0x2112;
          if (family == 0x01 && attr_len >= 8) {
            std::uint32_t xaddr = (static_cast<std::uint32_t>(buf[off + 4]) << 24) |
                                  (static_cast<std::uint32_t>(buf[off + 5]) << 16) |
                                  (static_cast<std::uint32_t>(buf[off + 6]) << 8) |
                                  static_cast<std::uint32_t>(buf[off + 7]);
            if (attr_type == 0x0020) xaddr ^= 0x2112A442U;
            char ip[INET_ADDRSTRLEN]{};
            in_addr addr{};
            addr.s_addr = htonl(xaddr);
            inet_ntop(AF_INET, &addr, ip, sizeof(ip));
            net_close(fd);
            freeaddrinfo(res);
            return Endpoint{ip, xport};
          }
        }
      }
      off += (attr_len + 3) & ~std::size_t{3};
    }
  }

  net_close(fd);
  freeaddrinfo(res);
  return std::nullopt;
}

}  // namespace

std::string stun_nat_class_name(StunNatClass type) {
  switch (type) {
    case StunNatClass::Open:
      return "open";
    case StunNatClass::Cone:
      return "cone";
    case StunNatClass::Symmetric:
      return "symmetric";
    default:
      return "unknown";
  }
}

bool should_run_stun_probe(bool explicit_udp_probe, bool ai_route, bool ai_route_plan_only) {
  return explicit_udp_probe || ai_route || ai_route_plan_only;
}

StunProbeResult probe_stun_nat(std::chrono::milliseconds timeout) {
  StunProbeResult result;
  const auto half = std::chrono::milliseconds(timeout.count() / 2 > 0 ? timeout.count() / 2 : 1);
  auto a = stun_binding(kStunPrimary, kStunPort, half);
  auto b = stun_binding(kStunSecondary, kStunPort, half);
  if (!a && !b) {
    result.error = "stun unreachable";
    return result;
  }
  if (!a) a = b;
  if (!b) b = a;
  result.ok = true;
  result.mapped = *a;
  result.mapped_alt = *b;
  if (a->host == b->host && a->port == b->port) {
    const auto locals = local_lan_candidate_addresses();
    const bool mapped_is_local =
        std::find(locals.begin(), locals.end(), a->host) != locals.end() ||
        std::find(locals.begin(), locals.end(), b->host) != locals.end();
    result.nat_class = mapped_is_local ? StunNatClass::Open : StunNatClass::Cone;
  } else {
    result.nat_class = StunNatClass::Symmetric;
  }
  return result;
}

bool detect_vpn_interfaces() {
#ifndef _WIN32
  ifaddrs* ifaddr = nullptr;
  if (getifaddrs(&ifaddr) != 0) return false;
  bool found = false;
  for (ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
    if ((ifa->ifa_flags & IFF_UP) == 0) continue;
    if (is_vpn_interface_name(ifa->ifa_name ? ifa->ifa_name : "")) {
      found = true;
      break;
    }
  }
  freeifaddrs(ifaddr);
  return found;
#else
  return false;
#endif
}

bool is_vpn_interface_name(std::string_view name) {
  return name.rfind("tun", 0) == 0 || name.rfind("wg", 0) == 0 || name.rfind("utun", 0) == 0 ||
         name.rfind("ppp", 0) == 0 || name.rfind("ipsec", 0) == 0;
}

bool is_likely_virtual_interface_name(std::string_view name) {
  return name.empty() || is_vpn_interface_name(name) || name.rfind("lo", 0) == 0 || name.rfind("awdl", 0) == 0 ||
         name.rfind("llw", 0) == 0 || name.rfind("bridge", 0) == 0 || name.rfind("vmenet", 0) == 0 ||
         name.rfind("vmnet", 0) == 0 || name.rfind("docker", 0) == 0 || name.rfind("br-", 0) == 0 ||
         name.rfind("gif", 0) == 0 || name.rfind("stf", 0) == 0;
}

std::optional<std::string> choose_physical_interface_name() {
  struct Candidate {
    std::string name;
    int score = 0;
  };

  std::map<std::string, Candidate> candidates;
  for (const auto& iface : collect_interface_addresses()) {
    if (iface.loopback || iface.vpn || is_likely_virtual_interface_name(iface.name)) continue;
    auto& candidate = candidates[iface.name];
    candidate.name = iface.name;
    if (iface.address.find('.') != std::string::npos) candidate.score += 20;
    if (iface.name == "en0") candidate.score += 120;
    else if (iface.name == "en1") candidate.score += 110;
    else if (iface.name.rfind("en", 0) == 0) candidate.score += 90;
    else if (iface.name.rfind("eth", 0) == 0) candidate.score += 80;
    else if (iface.name.rfind("wl", 0) == 0) candidate.score += 80;
    else candidate.score += 10;
  }

  std::optional<Candidate> best;
  for (const auto& [_, candidate] : candidates) {
    if (!best || candidate.score > best->score) best = candidate;
  }
  if (!best) return std::nullopt;
  return best->name;
}

std::vector<InterfaceAddress> collect_interface_addresses() {
  std::vector<InterfaceAddress> out;
#ifndef _WIN32
  ifaddrs* ifaddr = nullptr;
  if (getifaddrs(&ifaddr) != 0) return out;

  auto push_unique = [&](InterfaceAddress entry) {
    if (entry.address.empty()) return;
    for (const auto& existing : out) {
      if (existing.name == entry.name && existing.address == entry.address) return;
    }
    out.push_back(std::move(entry));
  };

  for (ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == nullptr) continue;
    if ((ifa->ifa_flags & IFF_UP) == 0) continue;

    char host[INET6_ADDRSTRLEN] = {};
    const int family = ifa->ifa_addr->sa_family;
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

    const std::string name = ifa->ifa_name ? ifa->ifa_name : "";
    push_unique(InterfaceAddress{name, host, is_vpn_interface_name(name), (ifa->ifa_flags & IFF_LOOPBACK) != 0});
  }
  freeifaddrs(ifaddr);
#else
  for (const auto& address : local_interface_addresses()) {
    out.push_back(InterfaceAddress{"", address, false, false});
  }
#endif
  return out;
}

}  // namespace kiko
