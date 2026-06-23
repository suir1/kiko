#include "relay_race.hpp"

#include "platform.hpp"

#include <algorithm>
#include <future>
#include <thread>

namespace kiko {
namespace {

constexpr auto kRelayProbeReadTimeout = std::chrono::milliseconds(500);
constexpr auto kControlFrameReadTimeout = std::chrono::milliseconds(1500);

std::string relay_kind_for_entry(const RelayRaceEntry& entry, const Endpoint& external_relay) {
  if (entry.endpoint.host == external_relay.host && entry.endpoint.port == external_relay.port) return "external";
  if (entry.endpoint.host == "127.0.0.1" || entry.endpoint.host == "::1") return "embedded";
  return "lan";
}

bool is_loopback_host(const std::string& host) {
  return host == "127.0.0.1" || host == "::1" || host == "localhost";
}

ConnectOptions connect_options_for_entry(const RelayRaceEntry& entry, const ConnectOptions& base) {
  ConnectOptions options;
  if (entry.use_proxy) options.proxy = base.proxy;
  if (!is_loopback_host(entry.endpoint.host)) options.bind_interface = base.bind_interface;
  return options;
}

std::optional<Endpoint> probe_punch_mapping(const Endpoint& relay, const Message& hello,
                                            const ConnectOptions& connect_options,
                                            const std::optional<std::string>& relay_pass,
                                            std::chrono::milliseconds timeout,
                                            const std::atomic_bool* cancel) {
  if (connect_options.proxy || timeout.count() <= 0) return std::nullopt;
  if (cancel && cancel->load()) return std::nullopt;

  const auto listen_port = hello.get_u64("listen_port", 0);
  if (listen_port == 0 || listen_port > 65535) return std::nullopt;

  ConnectOptions probe_options = connect_options;
  probe_options.local_bind = Endpoint{"", static_cast<std::uint16_t>(listen_port)};

  auto socket = connect_tcp(relay, timeout, probe_options, cancel);
  if (!socket.valid()) return std::nullopt;

  Message probe{"punch_probe", {{"room", hello.get("room")}, {"role", hello.get("role")}}};
  if (relay_pass && !relay_pass->empty()) probe.fields["relay_pass"] = *relay_pass;

  try {
    send_message(socket, probe);
    const auto observed = recv_message_timeout(socket, std::min(timeout, kControlFrameReadTimeout), cancel);
    if (!observed || observed->type != "punch_observed") return std::nullopt;
    const auto public_host = observed->get("public_host");
    const auto public_port = observed->get_u64("public_port", 0);
    if (public_host.empty() || public_port == 0 || public_port > 65535) return std::nullopt;
    return Endpoint{public_host, static_cast<std::uint16_t>(public_port)};
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

}  // namespace

std::optional<TcpSocket> try_connect_relay_and_register(const Endpoint& relay, const Message& hello,
                                                        const ConnectOptions& connect_options,
                                                        const std::optional<std::string>& relay_pass,
                                                        std::chrono::milliseconds timeout,
                                                        const std::atomic_bool* cancel) {
  if (cancel && cancel->load()) return std::nullopt;
  Message registration = hello;
  if (relay_pass && !relay_pass->empty()) registration.fields["relay_pass"] = *relay_pass;

  if (timeout.count() <= 0) timeout = std::chrono::milliseconds(1);
  const auto deadline = std::chrono::steady_clock::now() + timeout;

  const auto probe_budget = std::min<std::chrono::milliseconds>(timeout, std::chrono::milliseconds(600));
  if (auto punch_mapping = probe_punch_mapping(relay, hello, connect_options, relay_pass, probe_budget, cancel)) {
    registration.fields["punch_public_host"] = punch_mapping->host;
    registration.fields["punch_public_port"] = std::to_string(punch_mapping->port);
  }

  auto socket = connect_tcp(relay, timeout, connect_options, cancel);
  if (!socket.valid()) return std::nullopt;

  auto remaining_until_deadline = [&]() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
  };

  try {
    send_message(socket, Message{"ping", {}});
  } catch (const std::exception&) {
    socket.close();
    return std::nullopt;
  }
  bool pong_ok = false;
  while ((!cancel || !cancel->load()) && std::chrono::steady_clock::now() < deadline) {
    const int fd = socket.fd();
    if (fd < 0) break;
    auto remaining = remaining_until_deadline();
    if (remaining.count() <= 0) break;
    if (net_poll(fd, true, false, static_cast<int>(std::min<std::int64_t>(remaining.count(), 50))) <= 0) continue;
    try {
      const auto read_timeout = std::min<std::chrono::milliseconds>(remaining, kControlFrameReadTimeout);
      if (auto msg = recv_message_timeout(socket, read_timeout, cancel)) {
        if (msg->type == "pong") {
          pong_ok = true;
          break;
        }
        if (msg->type == "error") {
          socket.close();
          return std::nullopt;
        }
      }
    } catch (const KikoError&) {
      socket.close();
      return std::nullopt;
    }
  }
  if (!pong_ok || (cancel && cancel->load())) {
    socket.close();
    return std::nullopt;
  }

  if (remaining_until_deadline().count() <= 0 || (cancel && cancel->load())) {
    socket.close();
    return std::nullopt;
  }

  try {
    send_message(socket, registration);
  } catch (const std::exception&) {
    socket.close();
    return std::nullopt;
  }
  return socket;
}

std::optional<RelayPeerResult> wait_for_peer_messages(std::vector<TcpSocket>& relays,
                                                      const std::vector<Endpoint>& relay_endpoints,
                                                      std::chrono::milliseconds timeout,
                                                      const std::atomic_bool* cancel) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while ((!cancel || !cancel->load()) && std::chrono::steady_clock::now() < deadline) {
    const auto now = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < relays.size(); ++i) {
      if (!relays[i].valid()) continue;
      const int fd = relays[i].fd();
      if (fd < 0) continue;
      auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
      if (remaining.count() <= 0) break;
      const int poll_ms = static_cast<int>(std::min<std::int64_t>(remaining.count(), 50));
      if (net_poll(fd, /*want_read=*/true, /*want_write=*/false, poll_ms) <= 0) continue;
      const auto read_timeout = std::min<std::chrono::milliseconds>(remaining, kControlFrameReadTimeout);
      std::optional<Message> msg;
      try {
        msg = recv_message_timeout(relays[i], read_timeout, cancel);
      } catch (const KikoError&) {
        relays[i].close();
        continue;
      }
      if (msg) {
        if (msg->type == "error") {
          relays[i].close();
          continue;
        }
        if (msg->type != "peer") continue;
        RelayPeerResult result{std::move(relays[i]), std::move(*msg), relay_endpoints[i]};
        for (std::size_t j = 0; j < relays.size(); ++j) {
          if (j != i) relays[j].close();
        }
        relays.clear();
        return result;
      }
      relays[i].close();
    }
  }
  return std::nullopt;
}

std::optional<RelayPeerResult> race_until_peer(const std::vector<RelayRaceEntry>& entries, const Message& hello,
                                               std::chrono::milliseconds deadline,
                                               const ConnectOptions& connect_options,
                                               const std::optional<std::string>& relay_pass,
                                               const std::atomic_bool* cancel) {
  if (entries.empty()) return std::nullopt;
  if (cancel && cancel->load()) return std::nullopt;

  struct ConnectOutcome {
    Endpoint endpoint;
    std::optional<TcpSocket> socket;
  };

  std::vector<std::future<ConnectOutcome>> futures;
  futures.reserve(entries.size());
  auto connect_timeout = std::min<std::chrono::milliseconds>(deadline, std::chrono::seconds(5));
  if (connect_timeout.count() <= 0) connect_timeout = std::chrono::milliseconds(1);
  for (const auto& entry : entries) {
    futures.push_back(std::async(std::launch::async, [entry, hello, connect_options, relay_pass, connect_timeout, cancel]() {
      ConnectOutcome outcome{entry.endpoint, std::nullopt};
      outcome.socket =
          try_connect_relay_and_register(entry.endpoint, hello, connect_options_for_entry(entry, connect_options),
                                         relay_pass, connect_timeout, cancel);
      return outcome;
    }));
  }

  const auto end = std::chrono::steady_clock::now() + deadline;
  std::vector<TcpSocket> relays;
  std::vector<Endpoint> relay_endpoints;
  std::vector<bool> future_done(futures.size(), false);

  while ((!cancel || !cancel->load()) && std::chrono::steady_clock::now() < end) {
    for (std::size_t i = 0; i < futures.size(); ++i) {
      if (future_done[i]) continue;
      if (futures[i].wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) continue;
      future_done[i] = true;
      auto outcome = futures[i].get();
      if (outcome.socket && outcome.socket->valid()) {
        relay_endpoints.push_back(outcome.endpoint);
        relays.push_back(std::move(*outcome.socket));
      }
    }

    if (!relays.empty()) {
      auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now());
      if (remaining.count() <= 0) break;
      const auto poll_budget = std::min<std::chrono::milliseconds>(remaining, kControlFrameReadTimeout);
      if (auto peer = wait_for_peer_messages(relays, relay_endpoints, poll_budget, cancel)) return peer;
    }

    if (std::all_of(future_done.begin(), future_done.end(), [](bool done) { return done; }) && relays.empty()) {
      return std::nullopt;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  if (cancel && cancel->load()) return std::nullopt;
  if (relays.empty()) return std::nullopt;
  auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now());
  if (remaining.count() <= 0) remaining = std::chrono::milliseconds(1);
  return wait_for_peer_messages(relays, relay_endpoints, remaining, cancel);
}

std::int64_t probe_relay_rtt_ms(const Endpoint& relay, const ConnectOptions& connect_options,
                                std::chrono::milliseconds timeout) {
  const auto start = now_ms();
  auto sock = connect_tcp(relay, timeout, connect_options);
  if (!sock.valid()) return -1;
  try {
    send_message(sock, Message{"ping", {}});
    auto pong = recv_message_timeout(sock, std::min(timeout, kRelayProbeReadTimeout));
    if (!pong || pong->type != "pong") return -1;
  } catch (const std::exception&) {
    return -1;
  }
  return static_cast<std::int64_t>(now_ms() - start);
}

std::vector<RelayProbeEntry> probe_and_sort_relay_race_entries(std::vector<RelayRaceEntry>& entries,
                                                               const Endpoint& external_relay,
                                                               const ConnectOptions& connect_options) {
  if (entries.empty()) return {};

  struct ScoredEntry {
    RelayRaceEntry entry;
    std::int64_t rtt_ms = -1;
  };

  std::vector<std::future<ScoredEntry>> futures;
  futures.reserve(entries.size());
  for (const auto& entry : entries) {
    futures.push_back(std::async(std::launch::async, [entry, connect_options]() {
      return ScoredEntry{entry, probe_relay_rtt_ms(entry.endpoint, connect_options_for_entry(entry, connect_options))};
    }));
  }

  std::vector<ScoredEntry> scored;
  scored.reserve(futures.size());
  for (auto& future : futures) scored.push_back(future.get());

  std::stable_sort(scored.begin(), scored.end(), [](const ScoredEntry& a, const ScoredEntry& b) {
    if (a.rtt_ms < 0 && b.rtt_ms < 0) return false;
    if (a.rtt_ms < 0) return false;
    if (b.rtt_ms < 0) return true;
    return a.rtt_ms < b.rtt_ms;
  });

  entries.clear();
  entries.reserve(scored.size());
  std::vector<RelayProbeEntry> probes;
  probes.reserve(scored.size());
  for (const auto& item : scored) {
    entries.push_back(item.entry);
    RelayProbeEntry probe;
    probe.kind = relay_kind_for_entry(item.entry, external_relay);
    probe.endpoint = item.entry.endpoint.to_string();
    probe.rtt_ms = item.rtt_ms;
    probe.pong_ok = item.rtt_ms >= 0;
    probes.push_back(std::move(probe));
  }
  return probes;
}

void apply_relay_kind_order(std::vector<RelayRaceEntry>& entries, const std::vector<std::string>& kind_order,
                            const Endpoint& external_relay) {
  if (kind_order.empty() || entries.size() < 2) return;

  auto rank = [&](const std::string& kind) {
    for (std::size_t i = 0; i < kind_order.size(); ++i) {
      if (kind_order[i] == kind) return i;
    }
    return kind_order.size();
  };

  std::stable_sort(entries.begin(), entries.end(), [&](const RelayRaceEntry& a, const RelayRaceEntry& b) {
    return rank(relay_kind_for_entry(a, external_relay)) < rank(relay_kind_for_entry(b, external_relay));
  });
}

}  // namespace kiko
