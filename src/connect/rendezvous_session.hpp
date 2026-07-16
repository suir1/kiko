#pragma once

#include "core/common.hpp"
#include "core/network_interfaces.hpp"
#include "relay/relay_race.hpp"

#include <optional>
#include <vector>

namespace kiko {

void push_unique_endpoint(std::vector<Endpoint>& out, const Endpoint& ep);

[[nodiscard]] Endpoint relay_with_manual_ip(const Endpoint& relay, const std::optional<std::string>& manual_ip);

[[nodiscard]] std::vector<std::string> local_candidates_for_listener(
    const Endpoint& local_listen, const NetworkInterfaceInventory& interfaces);

[[nodiscard]] std::vector<RelayRaceEntry> relay_race_entries_for_send(bool use_embedded, const Endpoint& embedded_ep,
                                                                      bool only_local,
                                                                      const Endpoint& external_relay);

[[nodiscard]] std::vector<RelayRaceEntry> relay_race_entries_for_recv(const std::vector<Endpoint>& relay_targets,
                                                                      const Endpoint& external_relay);

}  // namespace kiko
