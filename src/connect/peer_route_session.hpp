#pragma once

#include "connect/connectivity.hpp"
#include "connect/peer_options.hpp"
#include "connect/route_session.hpp"
#include "core/crypto.hpp"
#include "core/progress.hpp"
#include "relay/relay_protocol.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace kiko {

class BackgroundRelay;

struct PeerRouteSessionConfig {
  PeerConnectionOptions connection;
  Role role = Role::Sender;
  std::string code;
  bool show_qrcode = true;
  std::string app;
  bool run_stun_probe = false;
  bool use_profile = false;
  std::string failure_message;
};

struct EstablishedPeerRoute {
  std::vector<TcpSocket> channels;
  SessionKey key;
  RoutePlan route_plan;
  RoutePath path = RoutePath::Relay;
  RouteOutcome outcome;
  PunchStats punch_stats;
  bool explain_direct_failure = false;
  bool mux_enabled = false;
  std::shared_ptr<BackgroundRelay> relay_keepalive;
};

class PeerRouteSession {
 public:
  PeerRouteSession(PeerRouteSessionConfig config, ProgressReporter& reporter);
  PeerRouteSession(const PeerRouteSession&) = delete;
  PeerRouteSession& operator=(const PeerRouteSession&) = delete;
  PeerRouteSession(PeerRouteSession&&) noexcept;
  PeerRouteSession& operator=(PeerRouteSession&&) noexcept;
  ~PeerRouteSession();

  [[nodiscard]] const std::string& code() const;
  [[nodiscard]] std::int64_t probe_external_relay_rtt() const;
  [[nodiscard]] const std::optional<StunProbeResult>& stun_probe() const;
  [[nodiscard]] ConnectivitySnapshot pre_rendezvous_snapshot(std::uint64_t total_bytes) const;
  void apply_relay_order(const std::vector<std::string>& relay_order);

  void rendezvous(RelayHello hello = {});
  [[nodiscard]] const RelayPeerInfo& peer() const;
  [[nodiscard]] ConnectivitySnapshot connectivity_snapshot(std::uint64_t total_bytes) const;

  [[nodiscard]] EstablishedPeerRoute establish(RoutePlan route_plan, int connections = 1,
                                               std::chrono::milliseconds mux_setup_timeout =
                                                   std::chrono::seconds(5));
  void record_success(const EstablishedPeerRoute& established);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace kiko
