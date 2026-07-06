#include "peer_session.hpp"

#include "connect/connectivity.hpp"
#include "connect/connectivity_session.hpp"
#include "connect/discovery.hpp"
#include "connect/lan_upgrade.hpp"
#include "connect/rendezvous_session.hpp"
#include "connect/route_planner.hpp"
#include "core/cancellation.hpp"
#include "core/pake.hpp"
#include "diagnostics/network_probe.hpp"
#include "diagnostics/outbound_policy.hpp"
#include "relay/relay_race.hpp"
#include "relay/relay_server.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

namespace kiko {
namespace {

constexpr auto kPeerRouteConfirmTimeout = std::chrono::seconds(20);

int elapsed_ms_since(std::chrono::steady_clock::time_point start) {
  const auto elapsed = std::chrono::steady_clock::now() - start;
  return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

const std::atomic_bool* cancel_flag(const std::shared_ptr<TransferCancellation>& cancellation) {
  return cancellation ? cancellation->flag() : nullptr;
}

void throw_if_cancelled(const std::shared_ptr<TransferCancellation>& cancellation) {
  if (cancellation && cancellation->requested()) throw KikoError("session canceled");
}

void track_socket(const std::shared_ptr<TransferCancellation>& cancellation, TcpSocket& socket) {
  if (cancellation) cancellation->track(socket);
}

std::size_t peer_global_ipv6_count(const Message& peer) {
  auto hosts = split_csv(peer.get("peer_local_candidates"));
  const auto listen_host = peer.get("peer_listen_host");
  if (!listen_host.empty()) hosts.push_back(listen_host);
  const auto public_host = peer.get("peer_public_host");
  if (!public_host.empty()) hosts.push_back(public_host);
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

std::string describe_peer_route_plan(const RoutePlan& plan) {
  std::string line = plan.reason;
  if (plan.skip_direct) return line + " (skip direct)";
  line += " direct_window=" + std::to_string(plan.direct_timeout.count()) + "ms";
  line += " direct_connect=" + std::to_string(plan.direct_connect.count()) + "ms";
  if (plan.udp_punch_enabled) line += " udp-assist";
  return line;
}

}  // namespace

PeerSession open_peer_session(const PeerSessionConfig& config, ProgressReporter& reporter) {
  throw_if_cancelled(config.cancellation);
  const bool is_host = config.role == Role::Sender;
  const auto code = is_host && config.code.empty() ? random_code(3) : config.code;
  if (auto error = validate_pairing_code_format(code, true)) throw KikoError(*error);

  auto listener = TcpListener::bind(config.listen);
  auto local_listen = listener.local_endpoint();
  reporter.status("listening for direct peer on " + local_listen.to_string());
  if (is_host) reporter.code_ready(code, config.show_qrcode);

  std::shared_ptr<BackgroundRelay> embedded;
  Endpoint embedded_endpoint;
  const bool use_embedded = is_host && config.lan_discover && !config.disable_local && !config.manual_ip;
  if (use_embedded) {
    embedded = std::make_shared<BackgroundRelay>();
    embedded->start(Endpoint{"0.0.0.0", 0});
    embedded_endpoint = embedded->local_endpoint();
  }

  std::atomic<bool> stop_lan{false};
  std::thread lan_thread;
  if (is_host && config.lan_discover && !config.manual_ip) {
    const auto announce_port = use_embedded ? embedded_endpoint.port : local_listen.port;
    if (announce_port > 0) lan_thread = std::thread([&]() { lan_announce(announce_port, stop_lan); });
  }
  LanAnnounceCleanup lan_cleanup(stop_lan, lan_thread);

  const auto external_relay = relay_with_manual_ip(config.relay, config.manual_ip);
  const auto outbound_selection =
      select_outbound_for_relay(external_relay, config.proxy, config.bind_interface, config.avoid_vpn);
  emit_outbound_summary(outbound_selection, external_relay, reporter);
  const auto connect_options = outbound_selection.connect_options;

  std::optional<StunProbeResult> stun_early;
  std::future<StunProbeResult> stun_future;
  if (config.udp_probe) {
    stun_future = std::async(std::launch::async, [] { return probe_stun_nat(std::chrono::milliseconds(800)); });
    if (stun_future.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready) {
      stun_early = stun_future.get();
    }
  }

  auto local_addrs = local_lan_candidate_addresses();
  auto advertised_listen = local_listen;
  apply_manual_ip(local_addrs, advertised_listen, config.manual_ip);

  std::vector<Endpoint> lan_extra;
  std::vector<RelayRaceEntry> race_entries;
  if (is_host) {
    race_entries = relay_race_entries_for_send(use_embedded, embedded_endpoint, config.only_local, external_relay);
    if (race_entries.empty()) throw KikoError("only-local mode requires embedded relay");
  } else {
    std::vector<Endpoint> relay_targets;
    if (config.lan_discover && !config.disable_local && !config.manual_ip) {
      lan_extra = lan_discover(std::chrono::milliseconds(200));
      if (config.only_local) {
        relay_targets = lan_extra;
      } else {
        for (const auto& ep : lan_extra) push_unique_endpoint(relay_targets, ep);
        push_unique_endpoint(relay_targets, external_relay);
      }
    } else if (config.only_local) {
      throw KikoError("only-local mode requires LAN relay discovery");
    } else {
      relay_targets.push_back(external_relay);
    }
    if (relay_targets.empty()) throw KikoError("no relay endpoints to try");
    race_entries = relay_race_entries_for_recv(relay_targets, external_relay);
  }
  auto relay_probes = probe_and_sort_relay_race_entries(race_entries, external_relay, connect_options);

  Message hello{"hello",
                {{"room", room_token(code)},
                 {"role", role_name(config.role)},
                 {"listen_host", advertised_listen.host},
                 {"listen_port", std::to_string(local_listen.port)},
                 {"local_candidates", join_csv(local_addrs)},
                 {"no_direct", config.no_direct ? "1" : "0"},
                 {"app", config.app}}};
  if (is_host) hello.fields["conn_count"] = "1";
  if (stun_early && stun_early->ok) hello.fields["stun_nat"] = stun_nat_class_name(stun_early->nat_class);

  ConnectivityRendezvous rendezvous;
  rendezvous.entries = race_entries;
  rendezvous.hello = hello;
  rendezvous.connect_options = connect_options;
  rendezvous.relay_pass = config.relay_pass;
  rendezvous.deadline = config.pair_timeout;
  rendezvous.failure_message = "failed to connect relay or rendezvous peer";
  rendezvous.cancel = cancel_flag(config.cancellation);

  reporter.route_phase(RoutePhase::Rendezvous, RoutePhaseDetail{"waiting for peer via relay", {}, false});
  RouteTiming route_timing;
  const auto rendezvous_start = std::chrono::steady_clock::now();
  auto peer_result = wait_for_connectivity_peer(rendezvous);
  throw_if_cancelled(config.cancellation);
  route_timing.rendezvous_ms = elapsed_ms_since(rendezvous_start);
  lan_cleanup.stop_now();

  auto relay = std::move(peer_result.socket);
  track_socket(config.cancellation, relay);
  auto peer = std::move(peer_result.peer);
  const auto active_relay = peer_result.relay;
  reporter.status("rendezvous relay: " + active_relay.to_string());

  Endpoint reflexive{peer.get("your_public_host"), message_port_or(peer, "your_public_port", 0, true)};
  auto self_nat = classify_nat(local_addrs, reflexive);
  Endpoint peer_public{peer.get("peer_public_host"), message_port_or(peer, "peer_public_port", 0, true)};
  auto peer_nat = classify_nat(split_csv(peer.get("peer_local_candidates")), peer_public);
  reporter.status("nat: self=" + nat_type_name(self_nat.type) + " peer=" + nat_type_name(peer_nat.type));

  std::optional<StunProbeResult> stun;
  if (stun_early) {
    stun = stun_early;
  } else if (stun_future.valid()) {
    stun = stun_future.get();
  }
  if (stun && stun->ok) {
    reporter.status("stun nat: " + stun_nat_class_name(stun->nat_class) + " mapped=" + stun->mapped.to_string());
  }

  ConnectivitySnapshot snapshot =
      build_pre_rendezvous_snapshot(config.no_direct, config.only_local, lan_extra.size(), 0);
  snapshot.self_global_ipv6_count = count_global_ipv6_addresses(local_addrs);
  snapshot.peer_global_ipv6_count = peer_global_ipv6_count(peer);
  snapshot.self_nat = self_nat.type;
  snapshot.peer_nat = peer_nat.type;
  snapshot.relays = std::move(relay_probes);
  if (stun && stun->ok) snapshot.stun_nat = stun->nat_class;

  auto route_plan = build_route_plan(config.no_direct, snapshot, stun, 1);
  apply_peer_direct_policy(route_plan, peer);
  reporter.status("route plan: " + describe_peer_route_plan(route_plan));

  auto secure_relay = [&](TcpSocket relay_channel, bool allow_lan_upgrade, RouteTiming timing,
                          const RouteOutcome& outcome) {
    const bool peer_no_direct = peer.get("peer_no_direct") == "1";
    if (allow_lan_upgrade && config.lan_discover && !config.no_direct && !peer_no_direct) {
      relay_channel = resolve_relay_channel(config.role, std::move(relay_channel), listener, local_listen.port,
                                            local_addrs, config.no_direct);
    }
    return PeerSession{secure_encrypted_session(std::move(relay_channel), config.role, code, "relay", timing,
                                                reporter, config.cancellation.get()),
                       code,
                       outcome,
                       active_relay,
                       embedded};
  };

  AdaptivePuncher puncher;
  ConnectivitySession connectivity_session{
      config.role,
      listener,
      peer,
      lan_extra,
      self_nat,
      peer_nat,
      route_plan,
      room_token(code),
      connect_options,
      kPeerRouteConfirmTimeout,
      route_timing,
      cancel_flag(config.cancellation),
  };
  auto selected_route = select_connectivity_route(std::move(relay), connectivity_session, puncher, reporter);
  throw_if_cancelled(config.cancellation);
  const auto report = puncher.report();
  if (report != "no punch observations\n") reporter.connectivity_report(report);

  if (selected_route.path == RoutePath::Relay) {
    return secure_relay(std::move(selected_route.relay), selected_route.allow_lan_upgrade, selected_route.timing,
                        selected_route.outcome);
  }
  if (selected_route.direct) {
    return PeerSession{secure_encrypted_session(std::move(*selected_route.direct), config.role, code, "direct",
                                                selected_route.timing, reporter, config.cancellation.get()),
                       code,
                       selected_route.outcome,
                       active_relay,
                       embedded};
  }
  throw KikoError("route selection did not produce a usable channel");
}

}  // namespace kiko
