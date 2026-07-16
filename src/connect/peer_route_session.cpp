#include "connect/peer_route_session.hpp"

#include "connect/direct_session.hpp"
#include "connect/discovery.hpp"
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

OutboundHistory outbound_history_from_selection(const OutboundSelection& selection) {
  OutboundHistory history;
  history.path = selection.chosen_path;
  history.bind_interface = selection.connect_options.bind_interface;
  history.reason = selection.reason;
  for (const auto& probe : selection.probes) {
    if (!probe.path.empty() && probe.rtt_ms >= 0) history.rtt_by_path[probe.path] = probe.rtt_ms;
  }
  return history;
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
    auto auxiliary =
        connect_tcp(relay, std::chrono::seconds(5), aux_options, cancellation_flag(options.cancellation));
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
  std::shared_ptr<BackgroundRelay> embedded;
  std::atomic_bool stop_lan{false};
  std::thread lan_thread;
  Endpoint external_relay;
  std::string fingerprint;
  OutboundSelection outbound;
  std::future<StunProbeResult> stun_future;
  std::optional<StunProbeResult> stun_result;
  std::vector<std::string> local_addrs;
  Endpoint advertised_listen;
  std::vector<Endpoint> lan_extra;
  std::vector<RelayRaceEntry> race_entries;
  ConnectivitySnapshot base_snapshot;
  TcpSocket relay;
  std::optional<RelayPeerInfo> peer_info;
  Endpoint active_relay;
  RouteTiming route_timing;
  bool established = false;

  Impl(PeerRouteSessionConfig config_in, ProgressReporter& reporter_in)
      : config(std::move(config_in)), reporter(reporter_in) {}

  ~Impl() { stop_lan_now(); }

  void stop_lan_now() {
    stop_lan.store(true);
    if (lan_thread.joinable()) lan_thread.join();
  }

  void prepare() {
    const auto& connection = config.connection;
    throw_if_cancelled(connection.cancellation, "session canceled");
    const bool is_host = config.role == Role::Sender;
    code = is_host && config.code.empty() ? random_code(3) : normalize_pairing_code(config.code);
    if (auto error = validate_pairing_code_format(code, true)) throw KikoError(*error);

    listener = TcpListener::bind(connection.listen);
    const auto local_listen = listener.local_endpoint();
    reporter.status("listening for direct peer on " + local_listen.to_string());
    if (is_host) reporter.code_ready(code, config.show_qrcode);

    const bool use_embedded =
        is_host && connection.lan_discover && !connection.disable_local && !connection.manual_ip;
    Endpoint embedded_endpoint;
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
    const auto interfaces = collect_network_interface_inventory();
    std::optional<NetworkProfileEntry> profile;
    fingerprint = network_fingerprint(interfaces);
    if (config.use_profile) profile = load_profile(fingerprint);
    outbound = select_outbound_for_relay(
        external_relay, connection.proxy, connection.bind_interface, connection.avoid_vpn,
        profile ? outbound_history_from_profile(*profile) : std::nullopt, interfaces);
    emit_outbound_summary(outbound, external_relay, reporter);

    if (config.run_stun_probe) {
      stun_future = std::async(std::launch::async, [] { return probe_stun_nat(std::chrono::milliseconds(800)); });
      if (stun_future.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready) {
        stun_result = stun_future.get();
      }
    }

    local_addrs = local_candidates_for_listener(local_listen, interfaces);
    advertised_listen = local_listen;
    if (connection.manual_ip && !connection.manual_ip->empty()) {
      const auto manual = parse_endpoint(*connection.manual_ip, advertised_listen.port);
      advertised_listen.host = manual.host;
      local_addrs = {manual.host};
    }

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
    base_snapshot = build_pre_rendezvous_snapshot(connection.no_direct, connection.only_local, lan_extra.size(), 0,
                                                  interfaces);
    if (profile) base_snapshot.profile = *profile;
    base_snapshot.relays =
        probe_and_sort_relay_race_entries(race_entries, external_relay, outbound.connect_options);
  }

  ConnectivitySnapshot snapshot(std::uint64_t total_bytes) const {
    auto value = base_snapshot;
    value.total_bytes = total_bytes;
    if (stun_result && stun_result->ok) value.stun_nat = stun_result->nat_class;
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
  return probe_relay_rtt_ms(impl_->external_relay, impl_->outbound.connect_options);
}

const std::optional<StunProbeResult>& PeerRouteSession::stun_probe() const { return impl_->stun_result; }

ConnectivitySnapshot PeerRouteSession::pre_rendezvous_snapshot(std::uint64_t total_bytes) const {
  return impl_->snapshot(total_bytes);
}

void PeerRouteSession::apply_relay_order(const std::vector<std::string>& relay_order) {
  if (impl_->peer_info) throw KikoError("cannot reorder relays after rendezvous");
  apply_relay_kind_order(impl_->race_entries, relay_order, impl_->external_relay);
}

void PeerRouteSession::rendezvous(RelayHello hello) {
  if (impl_->peer_info) throw KikoError("peer rendezvous already completed");
  const auto& connection = impl_->config.connection;

  hello.room = room_token(impl_->code);
  hello.role = impl_->config.role;
  hello.listen = impl_->advertised_listen;
  hello.local_candidates = impl_->local_addrs;
  hello.no_direct = connection.no_direct;
  hello.app = impl_->config.app;
  hello.stun_nat.clear();
  if (impl_->stun_result && impl_->stun_result->ok) {
    hello.stun_nat = stun_nat_class_name(impl_->stun_result->nat_class);
  }

  const auto* cancel = cancellation_flag(connection.cancellation);
  const auto failure_message = impl_->config.failure_message.empty()
                                   ? "failed to connect relay or rendezvous peer"
                                   : impl_->config.failure_message;

  std::string waiting_for = "peer";
  if (impl_->config.app.empty()) waiting_for = impl_->config.role == Role::Sender ? "receiver" : "sender";
  impl_->reporter.route_phase(RoutePhase::Rendezvous,
                              RoutePhaseDetail{"waiting for " + waiting_for + " via relay", {}, false});
  const auto start = std::chrono::steady_clock::now();
  auto peer_result = race_until_peer(impl_->race_entries, hello, connection.pair_timeout,
                                     impl_->outbound.connect_options, connection.relay_pass, cancel);
  if (!peer_result) throw KikoError(failure_message);
  auto peer = std::move(*peer_result);
  throw_if_cancelled(connection.cancellation, "session canceled");
  impl_->route_timing.rendezvous_ms = elapsed_ms_since(start);
  impl_->stop_lan_now();

  impl_->relay = std::move(peer.socket);
  track_socket(connection, impl_->relay);
  impl_->peer_info = decode_relay_peer_info(peer.peer);
  impl_->active_relay = peer.relay;
  impl_->reporter.status("rendezvous relay: " + impl_->active_relay.to_string());

  impl_->base_snapshot.self_nat = classify_nat(impl_->local_addrs, impl_->peer_info->self_public).type;
  impl_->base_snapshot.peer_nat =
      classify_nat(impl_->peer_info->peer_local_candidates, impl_->peer_info->peer_public).type;
  impl_->base_snapshot.self_global_ipv6_count = count_global_ipv6_addresses(impl_->local_addrs);
  impl_->base_snapshot.peer_global_ipv6_count = peer_global_ipv6_count(*impl_->peer_info);
  impl_->reporter.status("nat: self=" + nat_type_name(impl_->base_snapshot.self_nat) +
                         " peer=" + nat_type_name(impl_->base_snapshot.peer_nat));

  if (!impl_->stun_result && impl_->stun_future.valid()) {
    impl_->stun_result = impl_->stun_future.get();
  }
  if (impl_->stun_result && impl_->stun_result->ok) {
    impl_->reporter.status("stun nat: " + stun_nat_class_name(impl_->stun_result->nat_class) +
                           " mapped=" + impl_->stun_result->mapped.to_string());
  }
}

const RelayPeerInfo& PeerRouteSession::peer() const {
  if (!impl_->peer_info) throw KikoError("peer rendezvous has not completed");
  return *impl_->peer_info;
}

ConnectivitySnapshot PeerRouteSession::connectivity_snapshot(std::uint64_t total_bytes) const {
  if (!impl_->peer_info) throw KikoError("peer rendezvous has not completed");
  return impl_->snapshot(total_bytes);
}

EstablishedPeerRoute PeerRouteSession::establish(RoutePlan route_plan, int connections,
                                                 std::chrono::milliseconds mux_setup_timeout) {
  if (!impl_->peer_info) throw KikoError("peer rendezvous has not completed");
  if (impl_->established) throw KikoError("peer route already established");
  if (connections < 1) connections = 1;
  apply_peer_direct_policy(route_plan, impl_->peer_info->peer_no_direct);
  impl_->reporter.status("route plan: " + describe_route_plan(route_plan, true));

  AdaptivePuncher puncher;
  const auto* cancel = cancellation_flag(impl_->config.connection.cancellation);
  auto direct_attempt = [&](const std::atomic_bool* direct_cancel) {
    return attempt_direct(impl_->config.role, impl_->listener, *impl_->peer_info, impl_->lan_extra, puncher,
                          NatProfile{impl_->base_snapshot.self_nat}, NatProfile{impl_->base_snapshot.peer_nat},
                          route_plan, room_token(impl_->code), impl_->outbound.connect_options, &impl_->reporter,
                          direct_cancel);
  };
  RouteSelection selected;
  if (impl_->peer_info->route_commit_v2) {
    selected = race_transfer_route(std::move(impl_->relay), direct_attempt, puncher, route_plan, impl_->reporter,
                                   kRouteConfirmationTimeout, impl_->route_timing, cancel);
  } else {
    selected = select_transfer_route(std::move(impl_->relay), direct_attempt(cancel), puncher, route_plan,
                                     impl_->reporter, kRouteConfirmationTimeout, impl_->route_timing, cancel);
  }
  throw_if_cancelled(impl_->config.connection.cancellation, "session canceled");
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
                                      impl_->advertised_listen.port, impl_->local_addrs,
                                      impl_->config.connection.no_direct);
    }
  } else if (selected.direct) {
    primary = std::move(*selected.direct);
    route_name = "direct";
  } else {
    throw KikoError("route selection did not produce a usable channel");
  }

  track_socket(impl_->config.connection, primary);
  impl_->reporter.route_phase(RoutePhase::Securing,
                              RoutePhaseDetail{"securing " + route_name + " channel", route_name,
                                               route_name == "relay"});
  const auto securing_start = std::chrono::steady_clock::now();
  auto key = perform_handshake(primary, impl_->config.role, impl_->code);
  auto timing = selected.timing;
  timing.securing_ms = elapsed_ms_since(securing_start);
  impl_->reporter.route_timing(timing);
  impl_->reporter.handshake_ok();

  EstablishedPeerRoute established;
  established.key = std::move(key);
  established.route_plan = route_plan;
  established.path = selected.path;
  established.outcome = selected.outcome;
  established.punch_stats = selected.punch_stats;
  established.explain_direct_failure = selected.explain_direct_failure;
  established.relay_keepalive = impl_->embedded;

  if (connections == 1) {
    established.channels.push_back(std::move(primary));
  } else if (selected.path == RoutePath::Relay) {
    impl_->reporter.status("opening " + std::to_string(connections) + " parallel relay connections");
    established.channels =
        open_relay_mux_channels(std::move(primary), impl_->config.role, impl_->active_relay,
                                room_token(impl_->code), connections, impl_->outbound.connect_options,
                                impl_->config.connection.relay_pass, impl_->config.connection);
    established.mux_enabled = true;
  } else {
    auto mux = negotiate_direct_mux_channels(std::move(primary), impl_->config.role, impl_->listener,
                                             *impl_->peer_info, connections, room_token(impl_->code),
                                             impl_->outbound.connect_options, mux_setup_timeout,
                                             cancellation_flag(impl_->config.connection.cancellation));
    track_sockets(impl_->config.connection, mux.channels);
    established.channels = std::move(mux.channels);
    established.mux_enabled = mux.mux_enabled;
    if (established.mux_enabled) {
      impl_->reporter.status("opening " + std::to_string(connections) + " parallel direct connections");
    } else {
      impl_->reporter.status("parallel direct unavailable, using single direct connection (" +
                             mux.fallback_reason + ")");
    }
  }

  impl_->established = true;
  return established;
}

void PeerRouteSession::record_success(const EstablishedPeerRoute& established) {
  if (!impl_->config.use_profile) return;
  const auto path = established.path == RoutePath::Direct ? "direct" : "relay";
  save_profile_success(
      impl_->fingerprint,
      ProfileSuccess{path, established.punch_stats, outbound_history_from_selection(impl_->outbound)});
}

}  // namespace kiko
