#pragma once

#include "core/common.hpp"
#include "core/proxy.hpp"
#include "core/socket.hpp"
#include "relay/relay_protocol.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace kiko {

struct RelayProbeEntry {
  std::string kind;  // embedded | lan | external
  std::string endpoint;
  std::int64_t rtt_ms = -1;
  bool pong_ok = false;
};

struct RelayPeerResult {
  TcpSocket socket;
  Message peer;
  Endpoint relay;
};

struct RelayRaceEntry {
  Endpoint endpoint;
  bool use_proxy = false;
  std::size_t priority = 0;
  std::chrono::milliseconds start_delay{0};
};

[[nodiscard]] std::optional<TcpSocket> try_connect_relay_and_register(const Endpoint& relay, const RelayHello& hello,
                                                                      const ConnectOptions& connect_options,
                                                                      const std::optional<std::string>& relay_pass = std::nullopt,
                                                                      std::chrono::milliseconds timeout =
                                                                          std::chrono::seconds(5),
                                                                      const std::atomic_bool* cancel = nullptr);

[[nodiscard]] std::optional<RelayPeerResult> race_until_peer(const std::vector<RelayRaceEntry>& entries,
                                                             const RelayHello& hello,
                                                             std::chrono::milliseconds deadline,
                                                             const ConnectOptions& connect_options,
                                                             const std::optional<std::string>& relay_pass = std::nullopt,
                                                             const std::atomic_bool* cancel = nullptr);

[[nodiscard]] std::int64_t probe_relay_rtt_ms(const Endpoint& relay,
                                              const ConnectOptions& connect_options = ConnectOptions{},
                                              std::chrono::milliseconds timeout = std::chrono::seconds(3));

[[nodiscard]] std::vector<RelayProbeEntry> probe_and_sort_relay_race_entries(std::vector<RelayRaceEntry>& entries,
                                                                             const Endpoint& external_relay,
                                                                             const ConnectOptions& connect_options);

// Reorders entries by kind priority (embedded/lan/external). Unknown kinds sort last.
void apply_relay_kind_order(std::vector<RelayRaceEntry>& entries, const std::vector<std::string>& kind_order,
                            const Endpoint& external_relay);

}  // namespace kiko
