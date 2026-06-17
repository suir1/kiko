#include "rendezvous_session.hpp"

namespace kiko {

LanAnnounceCleanup::LanAnnounceCleanup(std::atomic<bool>& stop, std::thread& worker) : stop_(stop), worker_(worker) {}

LanAnnounceCleanup::~LanAnnounceCleanup() { stop_now(); }

void LanAnnounceCleanup::stop_now() {
  stop_.store(true);
  if (worker_.joinable()) worker_.join();
}

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

void apply_manual_ip(std::vector<std::string>& local_addrs, Endpoint& listen,
                     const std::optional<std::string>& manual_ip) {
  if (!manual_ip || manual_ip->empty()) return;
  auto ep = parse_endpoint(*manual_ip, listen.port);
  listen.host = ep.host;
  local_addrs.clear();
  local_addrs.push_back(ep.host);
}

std::vector<RelayRaceEntry> relay_race_entries_for_send(bool use_embedded, const Endpoint& embedded_ep,
                                                        bool only_local, const Endpoint& external_relay) {
  std::vector<RelayRaceEntry> entries;
  if (use_embedded) entries.push_back(RelayRaceEntry{Endpoint{"127.0.0.1", embedded_ep.port}, false});
  if (!only_local) entries.push_back(RelayRaceEntry{external_relay, true});
  return entries;
}

std::vector<RelayRaceEntry> relay_race_entries_for_recv(const std::vector<Endpoint>& relay_targets,
                                                        const Endpoint& external_relay) {
  std::vector<RelayRaceEntry> entries;
  entries.reserve(relay_targets.size());
  for (const auto& target : relay_targets) {
    const bool external = target.host == external_relay.host && target.port == external_relay.port;
    entries.push_back(RelayRaceEntry{target, external});
  }
  return entries;
}

}  // namespace kiko
