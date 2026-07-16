#include "rendezvous_session.hpp"

namespace kiko {

void push_unique_endpoint(std::vector<Endpoint>& out, const Endpoint& ep) {
  if (ep.port == 0) return;
  for (const auto& existing : out) {
    if (existing.host == ep.host && existing.port == ep.port) return;
  }
  out.push_back(ep);
}

Endpoint relay_with_manual_ip(const Endpoint& relay, const std::optional<std::string>& manual_ip) {
  if (!manual_ip || manual_ip->empty()) return relay;
  return parse_endpoint(*manual_ip, relay.port);
}

std::vector<std::string> local_candidates_for_listener(const Endpoint& local_listen,
                                                       const NetworkInterfaceInventory& interfaces) {
  if (local_listen.host.empty() || local_listen.host == "0.0.0.0" || local_listen.host == "::" ||
      local_listen.host == "[::]") {
    return interfaces.lan_candidate_addresses();
  }
  return {local_listen.host};
}

std::vector<RelayRaceEntry> relay_race_entries_for_send(bool use_embedded, const Endpoint& embedded_ep,
                                                        bool only_local, const Endpoint& external_relay) {
  std::vector<RelayRaceEntry> entries;
  if (use_embedded) entries.push_back(RelayRaceEntry{Endpoint{"127.0.0.1", embedded_ep.port}, false, 0});
  if (!only_local) entries.push_back(RelayRaceEntry{external_relay, true, use_embedded ? 1u : 0u});
  return entries;
}

std::vector<RelayRaceEntry> relay_race_entries_for_recv(const std::vector<Endpoint>& relay_targets,
                                                        const Endpoint& external_relay) {
  std::vector<RelayRaceEntry> entries;
  entries.reserve(relay_targets.size());
  bool has_local_relay = false;
  for (const auto& target : relay_targets) {
    if (!(target.host == external_relay.host && target.port == external_relay.port)) {
      has_local_relay = true;
      break;
    }
  }
  for (const auto& target : relay_targets) {
    const bool external = target.host == external_relay.host && target.port == external_relay.port;
    entries.push_back(RelayRaceEntry{target,
                                     external,
                                     external ? 1u : 0u,
                                     external && has_local_relay ? std::chrono::milliseconds(650)
                                                                 : std::chrono::milliseconds(0)});
  }
  return entries;
}

}  // namespace kiko
