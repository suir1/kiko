#pragma once

#include "common.hpp"
#include "relay/relay_race.hpp"

#include <atomic>
#include <optional>
#include <thread>
#include <vector>

namespace kiko {

class LanAnnounceCleanup {
 public:
  LanAnnounceCleanup(std::atomic<bool>& stop, std::thread& worker);
  ~LanAnnounceCleanup();

  void stop_now();

 private:
  std::atomic<bool>& stop_;
  std::thread& worker_;
};

void push_unique_endpoint(std::vector<Endpoint>& out, const Endpoint& ep);

[[nodiscard]] Endpoint relay_with_manual_ip(const Endpoint& relay, const std::optional<std::string>& manual_ip);

void apply_manual_ip(std::vector<std::string>& local_addrs, Endpoint& listen,
                     const std::optional<std::string>& manual_ip);

[[nodiscard]] std::vector<RelayRaceEntry> relay_race_entries_for_send(bool use_embedded, const Endpoint& embedded_ep,
                                                                      bool only_local,
                                                                      const Endpoint& external_relay);

[[nodiscard]] std::vector<RelayRaceEntry> relay_race_entries_for_recv(const std::vector<Endpoint>& relay_targets,
                                                                      const Endpoint& external_relay);

}  // namespace kiko
