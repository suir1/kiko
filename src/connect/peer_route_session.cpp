#include "connect/peer_route_session.hpp"

#include "connect/connectivity_session.hpp"
#include "connect/direct_session.hpp"
#include "connect/discovery.hpp"
#include "connect/encrypted_session.hpp"
#include "connect/lan_upgrade.hpp"
#include "connect/profile.hpp"
#include "connect/rendezvous_session.hpp"
#include "connect/route_planner.hpp"
#include "core/cancellation.hpp"
#include "core/network_interfaces.hpp"
#include "core/pake.hpp"
#include "diagnostics/network_probe.hpp"
#include "diagnostics/outbound_policy.hpp"
#include "relay/relay_race.hpp"
#include "relay/relay_protocol.hpp"
#include "relay/relay_server.hpp"

#include <atomic>
#include <future>
#include <thread>
#include <utility>

namespace kiko {
namespace {

constexpr auto kRouteConfirmationTimeout = std::chrono::seconds(20);

int elapsed_ms_since(std::chrono::steady_clock::time_point start) {
  const auto elapsed = std::chrono::steady_clock::now() - start;
  return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

const std::atomic_bool* cancel_flag(const PeerConnectionOptions& options) {
  return options.cancellation ? options.cancellation->flag() : nullptr;
}

void throw_if_cancelled(const PeerConnectionOptions& options) {
  if (options.cancellation && options.cancellation->requested()) throw KikoError("session canceled");
}

void track_socket(const PeerConnectionOptions& options, TcpSocket& socket) {
  if (options.cancellation) options.cancellation->track(socket);
}

void track_sockets(const PeerConnectionOptions& options, std::vector<TcpSocket>& sockets) {
  if (!options.cancellation) return;
  for (auto& socket : sockets) options.cancellation->track(socket);
}

std::size_t peer_global_ipv6_count(const RelayPeerInfo& peer) {
  auto hosts = peer.peer_local_candidates;
  if (!peer.peer_listen.host.empty()) hosts.push_back(peer.peer_listen.host);
  if (!peer.peer_public.host.empty()) hosts.push_back(peer.peer_public.host);
  return count_global_ipv6_addresses(hosts);
}

void emit_outbound_summary(const OutboundSelection& selection, const Endpoint& relay, ProgressReporter& reporter) {
  if (!selection.probes.empty()) {
    std::string line = "outbound probe:";
    for (const auto& probe : selection.probes) {
      line += " " + probe.path;
      if (!probe.bind_interface.empty()) line += "/" + probe.bind_interface;
      line += "=" + (probe.pong_ok ? std::to_string(probe.rtt_ms) + "ms" : "fail");
    }
    reporter.status(line);
  }
  if (!selection.connect_options.bind_interface.empty()) {
    reporter.status("outbound interface: " + selection.connect_options.bind_interface + " (" + selection.reason + ")");
  } else if (!relay_target_is_local(relay)) {
    reporter.status("outbound path: default (" + selection.reason + ")");
  }
}

void emit_punch_report(const AdaptivePuncher& puncher, ProgressReporter& reporter) {
  const auto report = puncher.report();
  if (report != "no punch observations\n") reporter.connectivity_report(report);
}

ProfileRelayPath profile_relay_path_from(const OutboundSelection& selection) {
  ProfileRelayPath relay;
  relay.path = selection.chosen_path;
  relay.bind_interface = selection.connect_options.bind_interface;
  relay.reason = selection.reason;
  for (const auto& probe : selection.probes) {
    if (!probe.path.empty() && probe.rtt_ms >= 0) relay.rtt_by_path[probe.path] = probe.rtt_ms;
  }
  return relay;
}

bool is_loopback_host(const std::string& host) {
  return host == "127.0.0.1" || host == "::1" || host == "localhost";
}

std::vector<TcpSocket> open_relay_mux_channels(TcpSocket primary, Role role, const Endpoint& relay,
                                               const std::string& room, int connections,
                                               const ConnectOptions& connect_options,
                                               const std::optional<std::string>& relay_pass,
                                               const PeerConnectionOptions& options) {
  std::vector<TcpSocket> channels;
  channels.reserve(static_cast<std::size_t>(connections));
  channels.push_back(std::move(primary));
  track_socket(options, channels.back());

  auto aux_options = connect_options;
  if (is_loopback_host(relay.host)) aux_options.bind_interface.clear();
  for (int index = 1; index < connections; ++index) {
    auto auxiliary = connect_tcp(relay, std::chrono::seconds(5), aux_options, cancel_flag(options));
    if (!auxiliary.valid()) throw KikoError("failed to open auxiliary relay connection");
    track_socket(options, auxiliary);
    RelayHello hello;
    hello.room = room;
    hello.role = role;
    hello.auxiliary = true;
    hello.conn_index = static_cast<std::uint64_t>(index);
    hello.relay_pass = relay_pass;
    send_message(auxiliary, encode_relay_hello(hello));
    channels.push_back(std::move(auxiliary));
  }
  return channels;
}

}  // namespace

struct PeerRouteSession::Impl {
  PeerRouteSessionConfig config;
  ProgressReporter& reporter;
  std::string code;
  TcpListener listener;
  Endpoint local_listen;
  std::shared_ptr<BackgroundRelay> embedded;
  Endpoint embedded_endpoint;
  std::atomic_bool stop_lan{false};
  std::thread lan_thread;
  Endpoint external_relay;
  NetworkInterfaceInventory interfaces;
  std::string fingerprint;
  std::optional<NetworkProfileEntry> profile;
  OutboundSelection outbound;
  ConnectOptions connect_options;
  std::optional<StunProbeResult> stun_early;
  std::future<StunProbeResult> stun_future;
  std::optional<StunProbeResult> stun_result;
  std::vector<std::string> local_addrs;
  Endpoint advertised_listen;
  std::vector<Endpoint> lan_extra;
  std::vector<RelayRaceEntry> race_entries;
  std::vector<RelayProbeEntry> relay_probes;
  TcpSocket relay;
  std::optional<Message> peer_message;
  std::optional<RelayPeerInfo> peer_info;
  Endpoint active_relay;
  NatProfile self_nat;
  NatProfile peer_nat;
  RouteTiming route_timing;
  bool rendezvoused = false;
  bool established = false;
  bool profile_recorded = false;

  Impl(PeerRouteSessionConfig config_in, ProgressReporter& reporter_in)
      : config(std::move(config_in)), reporter(reporter_in) {}

  ~Impl() { stop_lan_now(); }

  void stop_lan_now() {
    stop_lan.store(true);
    if (lan_thread.joinable()) lan_thread.join();
  }

  void prepare() {
    const auto& connection = config.connection;
    throw_if_cancelled(connection);
    const bool is_host = config.role == Role::Sender;
    code = is_host && config.code.empty() ? random_code(3) : normalize_pairing_code(config.code);
    if (auto error = validate_pairing_code_format(code, true)) throw KikoError(*error);

    listener = TcpListener::bind(connection.listen);
    local_listen = listener.local_endpoint();
    reporter.status("listening for direct peer on " + local_listen.to_string());
    if (is_host) reporter.code_ready(code, config.show_qrcode);

    const bool use_embedded =
        is_host && connection.lan_discover && !connection.disable_local && !connection.manual_ip;
    if (use_embedded) {
      embedded = std::make_shared<BackgroundRelay>();
      embedded->start(Endpoint{"0.0.0.0", 0});
      embedded_endpoint = embedded->local_endpoint();
    }
    if (is_host && connection.lan_discover && !connection.manual_ip) {
      const auto announce_port = use_embedded ? embedded_endpoint.port : local_listen.port;
      if (announce_port > 0) lan_thread = std::thread([this, announce_port] { lan_announce(announce_port, stop_lan); });
    }

    external_relay = relay_with_manual_ip(connection.relay, connection.manual_ip);
    interfaces = collect_network_interface_inventory();
    fingerprint = network_fingerprint(interfaces);
    if (config.use_profile) profile = load_profile(fingerprint);
    outbound = select_outbound_for_relay(
        external_relay, connection.proxy, connection.bind_interface, connection.avoid_vpn,
        profile ? outbound_history_from_profile(*profile) : std::nullopt, interfaces);
    emit_outbound_summary(outbound, external_relay, reporter);
    connect_options = outbound.connect_options;

    if (config.run_stun_probe) {
      stun_future = std::async(std::launch::async, [] { return probe_stun_nat(std::chrono::milliseconds(800)); });
      if (stun_future.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready) {
        stun_early = stun_future.get();
      }
    }

    local_addrs = interfaces.lan_candidate_addresses();
    advertised_listen = local_listen;
    apply_manual_ip(local_addrs, advertised_listen, connection.manual_ip);

    if (is_host) {
      race_entries = relay_race_entries_for_send(use_embedded, embedded_endpoint, connection.only_local, external_relay);
      if (race_entries.empty()) throw KikoError("only-local mode requires embedded relay");
    } else {
      std::vector<Endpoint> relay_targets;
      if (connection.lan_discover && !connection.disable_local && !connection.manual_ip) {
        lan_extra = lan_discover(std::chrono::milliseconds(200));
        if (connection.only_local) {
          relay_targets = lan_extra;
        } else {
          for (const auto& endpoint : lan_extra) push_unique_endpoint(relay_targets, endpoint);
          push_unique_endpoint(relay_targets, external_relay);
        }
      } else if (connection.only_local) {
        throw KikoError("only-local mode requires LAN relay discovery");
      } else {
        relay_targets.push_back(external_relay);
      }
      if (relay_targets.empty()) throw KikoError("no relay endpoints to try");
      race_entries = relay_race_entries_for_recv(relay_targets, external_relay);
    }
    relay_probes = probe_and_sort_relay_race_entries(race_entries, external_relay, connect_options);
  }

  ConnectivitySnapshot snapshot(std::uint64_t total_bytes, bool include_peer) const {
    auto value = build_pre_rendezvous_snapshot(config.connection.no_direct, config.connection.only_local,
                                               lan_extra.size(), total_bytes, interfaces);
    if (profile) apply_profile_to_snapshot(*profile, value);
    value.relays = relay_probes;
    const auto& observed_stun = include_peer ? stun_result : stun_early;
    if (observed_stun && observed_stun->ok) value.stun_nat = observed_stun->nat_class;
    if (!include_peer) return value;
    if (!peer_info) throw KikoError("peer rendezvous has not completed");
    value.self_global_ipv6_count = count_global_ipv6_addresses(local_addrs);
    value.peer_global_ipv6_count = peer_global_ipv6_count(*peer_info);
    value.self_nat = self_nat.type;
    value.peer_nat = peer_nat.type;
    return value;
  }
};

PeerRouteSession::PeerRouteSession(PeerRouteSessionConfig config, ProgressReporter& reporter)
    : impl_(std::make_unique<Impl>(std::move(config), reporter)) {
  impl_->prepare();
}

PeerRouteSession::PeerRouteSession(PeerRouteSession&&) noexcept = default;
PeerRouteSession& PeerRouteSession::operator=(PeerRouteSession&&) noexcept = default;
PeerRouteSession::~PeerRouteSession() = default;

const std::string& PeerRouteSession::code() const { return impl_->code; }

std::int64_t PeerRouteSession::probe_external_relay_rtt() const {
  return probe_relay_rtt_ms(impl_->external_relay, impl_->connect_options);
}

const std::optional<StunProbeResult>& PeerRouteSession::early_stun() const { return impl_->stun_early; }

ConnectivitySnapshot PeerRouteSession::pre_rendezvous_snapshot(std::uint64_t total_bytes) const {
  return impl_->snapshot(total_bytes, false);
}

void PeerRouteSession::apply_relay_order(const std::vector<std::string>& relay_order) {
  if (impl_->rendezvoused) throw KikoError("cannot reorder relays after rendezvous");
  apply_relay_kind_order(impl_->race_entries, relay_order, impl_->external_relay);
}

void PeerRouteSession::rendezvous(const std::map<std::string, std::string>& hello_fields) {
  if (impl_->rendezvoused) throw KikoError("peer rendezvous already completed");
  const auto& connection = impl_->config.connection;

  Message hello_message{"hello", hello_fields};
  hello_message.fields["room"] = room_token(impl_->code);
  hello_message.fields["role"] = role_name(impl_->config.role);
  hello_message.fields["listen_host"] = impl_->advertised_listen.host;
  hello_message.fields["listen_port"] = std::to_string(impl_->local_listen.port);
  hello_message.fields["local_candidates"] = join_csv(impl_->local_addrs);
  hello_message.fields["no_direct"] = connection.no_direct ? "1" : "0";
  hello_message.fields.erase("app");
  if (!impl_->config.app.empty()) hello_message.fields["app"] = impl_->config.app;
  hello_message.fields.erase("stun_nat");
  if (impl_->stun_early && impl_->stun_early->ok) {
    hello_message.fields["stun_nat"] = stun_nat_class_name(impl_->stun_early->nat_class);
  }

  ConnectivityRendezvous rendezvous;
  rendezvous.entries = impl_->race_entries;
  rendezvous.hello = encode_relay_hello(decode_relay_hello(hello_message));
  rendezvous.connect_options = impl_->connect_options;
  rendezvous.relay_pass = connection.relay_pass;
  rendezvous.deadline = connection.pair_timeout;
  rendezvous.failure_message = impl_->config.failure_message.empty()
                                   ? "failed to connect relay or rendezvous peer"
                                   : impl_->config.failure_message;
  rendezvous.cancel = cancel_flag(connection);

  std::string waiting_for = "peer";
  if (impl_->config.app.empty()) waiting_for = impl_->config.role == Role::Sender ? "receiver" : "sender";
  impl_->reporter.route_phase(RoutePhase::Rendezvous,
                              RoutePhaseDetail{"waiting for " + waiting_for + " via relay", {}, false});
  const auto start = std::chrono::steady_clock::now();
  auto peer_result = wait_for_connectivity_peer(rendezvous);
  throw_if_cancelled(connection);
  impl_->route_timing.rendezvous_ms = elapsed_ms_since(start);
  impl_->stop_lan_now();

  impl_->relay = std::move(peer_result.socket);
  track_socket(connection, impl_->relay);
  impl_->peer_info = decode_relay_peer_info(peer_result.peer);
  impl_->peer_message = std::move(peer_result.peer);
  impl_->active_relay = peer_result.relay;
  impl_->reporter.status("rendezvous relay: " + impl_->active_relay.to_string());

  impl_->self_nat = classify_nat(impl_->local_addrs, impl_->peer_info->self_public);
  impl_->peer_nat = classify_nat(impl_->peer_info->peer_local_candidates, impl_->peer_info->peer_public);
  impl_->reporter.status("nat: self=" + nat_type_name(impl_->self_nat.type) +
                         " peer=" + nat_type_name(impl_->peer_nat.type));

  if (impl_->stun_early) {
    impl_->stun_result = impl_->stun_early;
  } else if (impl_->stun_future.valid()) {
    impl_->stun_result = impl_->stun_future.get();
  }
  if (impl_->stun_result && impl_->stun_result->ok) {
    impl_->reporter.status("stun nat: " + stun_nat_class_name(impl_->stun_result->nat_class) +
                           " mapped=" + impl_->stun_result->mapped.to_string());
  }
  impl_->rendezvoused = true;
}

const Message& PeerRouteSession::peer() const {
  if (!impl_->peer_message) throw KikoError("peer rendezvous has not completed");
  return *impl_->peer_message;
}

const std::optional<StunProbeResult>& PeerRouteSession::stun() const {
  if (!impl_->rendezvoused) throw KikoError("peer rendezvous has not completed");
  return impl_->stun_result;
}

ConnectivitySnapshot PeerRouteSession::connectivity_snapshot(std::uint64_t total_bytes) const {
  if (!impl_->rendezvoused) throw KikoError("peer rendezvous has not completed");
  return impl_->snapshot(total_bytes, true);
}

RoutePlan PeerRouteSession::apply_peer_policy(RoutePlan plan) const {
  apply_peer_direct_policy(plan, peer());
  return plan;
}

EstablishedPeerRoute PeerRouteSession::establish(RoutePlan route_plan, int connections,
                                                 std::chrono::milliseconds mux_setup_timeout) {
  if (!impl_->rendezvoused) throw KikoError("peer rendezvous has not completed");
  if (impl_->established) throw KikoError("peer route already established");
  if (connections < 1) connections = 1;
  route_plan = apply_peer_policy(std::move(route_plan));

  AdaptivePuncher puncher;
  ConnectivitySession connectivity_session{
      impl_->config.role,
      impl_->listener,
      *impl_->peer_message,
      impl_->lan_extra,
      impl_->self_nat,
      impl_->peer_nat,
      route_plan,
      room_token(impl_->code),
      impl_->connect_options,
      kRouteConfirmationTimeout,
      impl_->route_timing,
      cancel_flag(impl_->config.connection),
  };
  auto selected =
      select_connectivity_route(std::move(impl_->relay), connectivity_session, puncher, impl_->reporter);
  throw_if_cancelled(impl_->config.connection);
  emit_punch_report(puncher, impl_->reporter);

  TcpSocket primary;
  std::string route_name;
  if (selected.path == RoutePath::Relay) {
    primary = std::move(selected.relay);
    route_name = "relay";
    const bool peer_no_direct = impl_->peer_info->peer_no_direct;
    if (selected.allow_lan_upgrade && impl_->config.connection.lan_discover &&
        !impl_->config.connection.no_direct && !peer_no_direct) {
      primary = resolve_relay_channel(impl_->config.role, std::move(primary), impl_->listener,
                                      impl_->local_listen.port, impl_->local_addrs,
                                      impl_->config.connection.no_direct);
    }
  } else if (selected.direct) {
    primary = std::move(*selected.direct);
    route_name = "direct";
  } else {
    throw KikoError("route selection did not produce a usable channel");
  }

  auto encrypted = secure_encrypted_session(std::move(primary), impl_->config.role, impl_->code, route_name,
                                            selected.timing, impl_->reporter,
                                            impl_->config.connection.cancellation.get());

  EstablishedPeerRoute established;
  established.key = std::move(encrypted.key);
  established.path = selected.path;
  established.outcome = selected.outcome;
  established.active_relay = impl_->active_relay;
  established.timing = encrypted.timing;
  established.punch_stats = selected.punch_stats;
  established.explain_direct_failure = selected.explain_direct_failure;
  established.relay_keepalive = impl_->embedded;
  if (impl_->config.use_profile && selected.path == RoutePath::Direct) {
    save_profile_success(impl_->fingerprint, "direct", established.punch_stats,
                         profile_relay_path_from(impl_->outbound));
    impl_->profile_recorded = true;
  }

  if (connections == 1) {
    established.channels.push_back(std::move(encrypted.channel));
  } else if (selected.path == RoutePath::Relay) {
    impl_->reporter.status("opening " + std::to_string(connections) + " parallel relay connections");
    established.channels =
        open_relay_mux_channels(std::move(encrypted.channel), impl_->config.role, impl_->active_relay,
                                room_token(impl_->code), connections, impl_->connect_options,
                                impl_->config.connection.relay_pass, impl_->config.connection);
    established.mux_enabled = true;
  } else {
    auto mux = negotiate_direct_mux_channels(std::move(encrypted.channel), impl_->config.role, impl_->listener,
                                             *impl_->peer_message, connections, room_token(impl_->code),
                                             impl_->connect_options, mux_setup_timeout,
                                             cancel_flag(impl_->config.connection));
    track_sockets(impl_->config.connection, mux.channels);
    established.channels = std::move(mux.channels);
    established.mux_enabled = mux.mux_enabled;
    established.mux_fallback_reason = std::move(mux.fallback_reason);
    if (established.mux_enabled) {
      impl_->reporter.status("opening " + std::to_string(connections) + " parallel direct connections");
    } else {
      impl_->reporter.status("parallel direct unavailable, using single direct connection (" +
                             established.mux_fallback_reason + ")");
    }
  }

  impl_->established = true;
  return established;
}

void PeerRouteSession::record_success(const EstablishedPeerRoute& established) {
  if (!impl_->config.use_profile || impl_->profile_recorded) return;
  const auto path = established.path == RoutePath::Direct ? "direct" : "relay";
  save_profile_success(impl_->fingerprint, path, established.punch_stats, profile_relay_path_from(impl_->outbound));
  impl_->profile_recorded = true;
}

}  // namespace kiko
