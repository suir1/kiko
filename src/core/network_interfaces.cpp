#include "core/network_interfaces.hpp"

#include "platform/platform.hpp"

#include <algorithm>
#include <map>
#include <utility>

namespace kiko {
namespace {

bool is_likely_virtual_interface_name(std::string_view name) {
  return name.empty() || is_vpn_interface_name(name) || name.rfind("lo", 0) == 0 || name.rfind("awdl", 0) == 0 ||
         name.rfind("llw", 0) == 0 || name.rfind("bridge", 0) == 0 || name.rfind("vmenet", 0) == 0 ||
         name.rfind("vmnet", 0) == 0 || name.rfind("docker", 0) == 0 || name.rfind("br-", 0) == 0 ||
         name.rfind("gif", 0) == 0 || name.rfind("stf", 0) == 0;
}

void push_unique(std::vector<std::string>& out, const std::string& value) {
  if (!value.empty() && std::find(out.begin(), out.end(), value) == out.end()) out.push_back(value);
}

void push_unique(std::vector<InterfaceAddress>& out, InterfaceAddress entry) {
  if (entry.address.empty()) return;
  const auto duplicate = std::find_if(out.begin(), out.end(), [&](const InterfaceAddress& existing) {
    return existing.name == entry.name && existing.address == entry.address;
  });
  if (duplicate == out.end()) out.push_back(std::move(entry));
}

}  // namespace

bool is_vpn_interface_name(std::string_view name) {
  return name.rfind("tun", 0) == 0 || name.rfind("wg", 0) == 0 || name.rfind("utun", 0) == 0 ||
         name.rfind("ppp", 0) == 0 || name.rfind("ipsec", 0) == 0;
}

std::vector<std::string> NetworkInterfaceInventory::non_loopback_addresses() const {
  std::vector<std::string> out;
  for (const auto& iface : interfaces) {
    if (!iface.loopback) push_unique(out, iface.address);
  }
  return out;
}

std::vector<std::string> NetworkInterfaceInventory::lan_candidate_addresses() const {
  std::vector<std::string> out;
  for (const auto& iface : interfaces) {
    if (!iface.loopback && !iface.vpn) push_unique(out, iface.address);
  }
  return out;
}

bool NetworkInterfaceInventory::vpn_detected() const {
  return vpn_interface_present ||
         std::any_of(interfaces.begin(), interfaces.end(),
                     [](const InterfaceAddress& iface) { return !iface.loopback && iface.vpn; });
}

std::optional<std::string> NetworkInterfaceInventory::preferred_physical_interface() const {
  std::map<std::string, int> scores;
  for (const auto& iface : interfaces) {
    if (iface.loopback || iface.vpn || is_likely_virtual_interface_name(iface.name)) continue;
    auto& score = scores[iface.name];
    if (iface.address.find('.') != std::string::npos) score += 20;
    if (iface.name == "en0") score += 120;
    else if (iface.name == "en1") score += 110;
    else if (iface.name.rfind("en", 0) == 0) score += 90;
    else if (iface.name.rfind("eth", 0) == 0 || iface.name.rfind("wl", 0) == 0) score += 80;
    else score += 10;
  }

  std::optional<std::pair<std::string, int>> best;
  for (const auto& candidate : scores) {
    if (!best || candidate.second > best->second) best = candidate;
  }
  if (!best) return std::nullopt;
  return best->first;
}

NetworkInterfaceInventory collect_network_interface_inventory() {
  NetworkInterfaceInventory inventory;
#ifdef _WIN32
  net_startup();
  ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
  ULONG size = 15 * 1024;
  std::vector<unsigned char> buffer(size);
  auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
  if (GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, adapters, &size) == ERROR_BUFFER_OVERFLOW) {
    buffer.resize(size);
    adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
  }
  if (GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, adapters, &size) != NO_ERROR) return inventory;

  for (auto* adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
    if (adapter->OperStatus != IfOperStatusUp) continue;
    const bool loopback = adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
    for (auto* ua = adapter->FirstUnicastAddress; ua != nullptr; ua = ua->Next) {
      char host[INET6_ADDRSTRLEN] = {};
      auto* sa = ua->Address.lpSockaddr;
      if (sa->sa_family == AF_INET) {
        auto* address = reinterpret_cast<sockaddr_in*>(sa);
        inet_ntop(AF_INET, &address->sin_addr, host, sizeof(host));
      } else if (sa->sa_family == AF_INET6) {
        auto* address = reinterpret_cast<sockaddr_in6*>(sa);
        if (IN6_IS_ADDR_LINKLOCAL(&address->sin6_addr)) continue;
        inet_ntop(AF_INET6, &address->sin6_addr, host, sizeof(host));
      } else {
        continue;
      }
      push_unique(inventory.interfaces, InterfaceAddress{"", host, false, loopback});
    }
  }
#else
  ifaddrs* ifaddr = nullptr;
  if (getifaddrs(&ifaddr) != 0) return inventory;
  for (ifaddrs* iface = ifaddr; iface != nullptr; iface = iface->ifa_next) {
    if ((iface->ifa_flags & IFF_UP) == 0) continue;
    const std::string name = iface->ifa_name ? iface->ifa_name : "";
    if (is_vpn_interface_name(name)) inventory.vpn_interface_present = true;
    if (iface->ifa_addr == nullptr) continue;
    char host[INET6_ADDRSTRLEN] = {};
    const int family = iface->ifa_addr->sa_family;
    if (family == AF_INET) {
      auto* address = reinterpret_cast<sockaddr_in*>(iface->ifa_addr);
      inet_ntop(AF_INET, &address->sin_addr, host, sizeof(host));
    } else if (family == AF_INET6) {
      auto* address = reinterpret_cast<sockaddr_in6*>(iface->ifa_addr);
      if (IN6_IS_ADDR_LINKLOCAL(&address->sin6_addr)) continue;
      inet_ntop(AF_INET6, &address->sin6_addr, host, sizeof(host));
    } else {
      continue;
    }
    push_unique(inventory.interfaces,
                InterfaceAddress{name, host, is_vpn_interface_name(name), (iface->ifa_flags & IFF_LOOPBACK) != 0});
  }
  freeifaddrs(ifaddr);
#endif
  return inventory;
}

}  // namespace kiko
