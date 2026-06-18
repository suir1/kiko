#include "transfer.hpp"

#include "adaptive.hpp"
#include "connectivity.hpp"
#include "crypto.hpp"
#include "direct_session.hpp"
#include "discovery.hpp"
#include "doctor.hpp"
#include "lan_upgrade.hpp"
#include "outbound_policy.hpp"
#include "pake.hpp"
#include "platform.hpp"
#include "profile.hpp"
#include "protocol.hpp"
#include "relay_race.hpp"
#include "rendezvous_session.hpp"
#include "relay_session.hpp"
#include "relay_server.hpp"
#include "route_planner.hpp"
#include "route_session.hpp"
#include "socket.hpp"
#include "transfer_heuristics.hpp"
#include "transfer_stream.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <limits>
#include <thread>

namespace kiko {

using namespace detail;

namespace {

void emit_punch_report(const AdaptivePuncher& puncher, ProgressReporter& reporter) {
  const auto report = puncher.report();
  if (report != "no punch observations\n") reporter.connectivity_report(report);
}

constexpr auto kRelayRouteConfirmTimeout = std::chrono::seconds(20);
constexpr auto kDirectMuxSetupTimeout = std::chrono::seconds(5);

int normalize_connection_count(int connections) {
  if (connections < 1) return 1;
  if (connections > kMaxMuxConnections) {
    throw KikoError("connection count exceeds maximum " + std::to_string(kMaxMuxConnections));
  }
  return connections;
}

ConnectOptions make_connect_options(const Endpoint& relay, const std::optional<ProxyConfig>& proxy,
                                    const std::string& bind_interface, bool avoid_vpn, ProgressReporter& reporter) {
  const auto selection = select_outbound_for_relay(relay, proxy, bind_interface, avoid_vpn);
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
  return selection.connect_options;
}

void emit_debug_route(const Endpoint& relay, const std::optional<ProxyConfig>& proxy,
                      const std::optional<std::string>& relay_pass, const std::string& bind_interface, bool avoid_vpn,
                      bool udp_probe, bool no_direct, bool only_local, ProgressReporter& reporter) {
  DoctorOptions options;
  options.relay = relay;
  options.proxy = proxy;
  options.relay_pass = relay_pass;
  options.bind_interface = bind_interface;
  options.avoid_vpn = avoid_vpn;
  options.udp_probe = udp_probe;
  options.no_direct = no_direct;
  options.only_local = only_local;
  const auto report = run_doctor(options);
  for (const auto& line : doctor_debug_lines(report)) reporter.status(line);
}

}  // namespace

int run_send(const SendConfig& config, ProgressReporter& reporter) {
  auto code = config.code.empty() ? random_code(3) : config.code;
  CollectOptions collect_opts;
  collect_opts.use_gitignore = config.use_gitignore;
  auto files = collect_files(config.file, collect_opts);
  std::uint64_t total_size = 0;
  for (const auto& entry : files) total_size += entry.size;

  auto listener = TcpListener::bind(config.listen);
  auto local_listen = listener.local_endpoint();

  reporter.code_ready(code, config.show_qrcode);
  reporter.transfer_overview(files.size(), total_size);
  reporter.status("listening for direct peer on " + local_listen.to_string());

  BackgroundRelay embedded;
  const bool use_embedded = config.lan_discover && !config.disable_local && !config.manual_ip;
  if (use_embedded) embedded.start(Endpoint{"0.0.0.0", 0});

  std::atomic<bool> stop_lan{false};
  std::thread lan_thread;
  if (config.lan_discover && !config.manual_ip) {
    const auto announce_port = use_embedded ? embedded.local_endpoint().port : local_listen.port;
    if (announce_port > 0) {
      lan_thread = std::thread([&]() { lan_announce(announce_port, stop_lan); });
    }
  }
  LanAnnounceCleanup lan_cleanup(stop_lan, lan_thread);

  int connections = normalize_connection_count(config.connections);
  const auto external_relay = relay_with_manual_ip(config.relay, config.manual_ip);
  const auto connect_options =
      make_connect_options(external_relay, config.proxy, config.bind_interface, config.avoid_vpn, reporter);
  if (config.auto_connections) {
    const auto rtt = probe_relay_rtt_ms(external_relay, connect_options);
    connections = normalize_connection_count(recommend_connections(rtt, total_size));
    reporter.status("auto connections: " + std::to_string(connections) +
                    (rtt >= 0 ? " (relay rtt " + std::to_string(rtt) + "ms)" : ""));
    if (config.ai_route && !config.ai_route_connectivity_only) {
      reporter.status("auto connections is a hint; --ai-route may override unless --ai-route-connectivity-only");
    }
  }

  const int connections_hint = connections;
  if (config.debug_route) {
    emit_debug_route(external_relay, config.proxy, config.relay_pass, config.bind_interface, config.avoid_vpn,
                     config.udp_probe, config.no_direct, config.only_local, reporter);
  }

  std::optional<StunProbeResult> stun_early;
  std::future<StunProbeResult> stun_future;
  const bool run_stun_probe = should_run_stun_probe(config.udp_probe, config.ai_route, config.ai_route_plan_only);
  if (run_stun_probe) {
    if (!config.udp_probe) reporter.status("ai route: running STUN NAT probe");
    stun_future = std::async(std::launch::async, [] { return probe_stun_nat(std::chrono::milliseconds(800)); });
    if (stun_future.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready) {
      stun_early = stun_future.get();
    }
  }

  auto local_addrs = local_lan_candidate_addresses();
  auto advertised_listen = local_listen;
  apply_manual_ip(local_addrs, advertised_listen, config.manual_ip);

  std::vector<RelayRaceEntry> race_entries =
      relay_race_entries_for_send(use_embedded, embedded.local_endpoint(), config.only_local, external_relay);
  if (race_entries.empty()) throw KikoError("only-local mode requires embedded relay");
  auto relay_probes = probe_and_sort_relay_race_entries(race_entries, external_relay, connect_options);

  std::optional<RoutePlan> pre_peer_ai_plan;
  if (config.ai_route || config.ai_route_plan_only) {
    ConnectivitySnapshot pre_snapshot =
        build_pre_rendezvous_snapshot(config.no_direct, config.only_local, 0, total_size);
    if (auto profile = load_profile(network_fingerprint())) apply_profile_to_snapshot(*profile, pre_snapshot);
    fill_transfer_snapshot(pre_snapshot, files, connections_hint);
    pre_snapshot.relays = relay_probes;
    if (stun_early && stun_early->ok) pre_snapshot.stun_nat = stun_early->nat_class;
    const auto pre_plan =
        resolve_route_plan(config.no_direct, pre_snapshot, stun_early, connections, config.ai_route,
                           config.ai_route_plan_only, config.ai_route_connectivity_only, reporter);
    if (config.ai_route && !config.ai_route_plan_only) {
      if (!config.ai_route_connectivity_only) connections = normalize_connection_count(pre_plan.connections);
      pre_peer_ai_plan = pre_plan;
      if (!pre_plan.relay_order.empty()) {
        apply_relay_kind_order(race_entries, pre_plan.relay_order, external_relay);
      }
    }
  }

  Message hello{"hello",
                {{"room", room_token(code)},
                 {"role", "sender"},
                 {"conn_index", "0"},
                 {"conn_count", std::to_string(connections)},
                 {"listen_host", advertised_listen.host},
                 {"listen_port", std::to_string(local_listen.port)},
                 {"local_candidates", join_csv(local_addrs)},
                 {"no_direct", config.no_direct ? "1" : "0"},
                 {"file_count", std::to_string(files.size())},
                 {"total_size", std::to_string(total_size)}}};
  if (stun_early && stun_early->ok) hello.fields["stun_nat"] = stun_nat_class_name(stun_early->nat_class);

  auto peer_result = race_until_peer(race_entries, hello, std::chrono::seconds(30), connect_options, config.relay_pass);
  lan_cleanup.stop_now();

  if (!peer_result) throw KikoError("failed to connect relay or rendezvous peer");
  auto relay = std::move(peer_result->socket);
  auto peer = std::move(peer_result->peer);
  const auto active_relay = peer_result->relay;

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

  ConnectivitySnapshot snapshot = build_pre_rendezvous_snapshot(config.no_direct, config.only_local, 0, total_size);
  if (auto profile = load_profile(network_fingerprint())) apply_profile_to_snapshot(*profile, snapshot);
  fill_transfer_snapshot(snapshot, files, connections_hint);
  snapshot.self_nat = self_nat.type;
  snapshot.peer_nat = peer_nat.type;
  snapshot.relays = std::move(relay_probes);
  if (stun && stun->ok) snapshot.stun_nat = stun->nat_class;

  RoutePlan route_plan = build_route_plan(config.no_direct, snapshot, stun, connections);
  if (pre_peer_ai_plan && config.ai_route && !config.ai_route_plan_only) {
    route_plan = merge_route_plan_hint(route_plan, *pre_peer_ai_plan);
    connections = normalize_connection_count(route_plan.connections);
  }
  apply_peer_direct_policy(route_plan, peer);
  reporter.status("route plan: " + route_plan.reason + (route_plan.skip_direct ? " (skip direct)" : ""));

  auto run_relay_send = [&](TcpSocket relay_channel, bool allow_lan_upgrade,
                            const std::optional<PunchStats>& profile_stats = std::nullopt) -> int {
    const bool peer_no_direct = peer.get("peer_no_direct") == "1";
    if (allow_lan_upgrade && config.lan_discover && !config.no_direct && !peer_no_direct) {
      relay_channel =
          resolve_relay_channel(Role::Sender, std::move(relay_channel), listener, local_listen.port, local_addrs, config.no_direct);
    }
    send_files_over_relay(std::move(relay_channel), active_relay, code, connections, connect_options, config.relay_pass,
                          files, reporter);
    if (profile_stats) save_profile_success(network_fingerprint(), "relay", *profile_stats);
    else save_profile_success(network_fingerprint(), "relay");
    return 0;
  };

  AdaptivePuncher puncher;
  auto direct =
      attempt_direct(Role::Sender, listener, peer, {}, puncher, self_nat, peer_nat, route_plan, room_token(code),
                     connect_options, &reporter);
  emit_punch_report(puncher, reporter);
  auto selected_route =
      select_transfer_route(std::move(relay), std::move(direct), puncher, route_plan, reporter, kRelayRouteConfirmTimeout);
  if (selected_route.path == RoutePath::Relay) {
    if (selected_route.explain_direct_failure) {
      snapshot.punch = selected_route.punch_stats;
      explain_direct_failure(snapshot, route_plan, config.ai_route, reporter);
    }
    return run_relay_send(std::move(selected_route.relay), selected_route.allow_lan_upgrade, selected_route.punch_stats);
  }

  if (selected_route.direct) {
    auto direct_channel = std::move(*selected_route.direct);
    auto key = perform_handshake(direct_channel, Role::Sender, code);
    reporter.handshake_ok();
    save_profile_success(network_fingerprint(), "direct", selected_route.punch_stats);
    if (connections > 1) {
      auto mux =
          negotiate_direct_mux_channels(std::move(direct_channel), Role::Sender, listener, peer, connections, room_token(code),
                                        connect_options, kDirectMuxSetupTimeout);
      if (mux.mux_enabled) {
        reporter.status("opening " + std::to_string(connections) + " parallel direct connections");
        send_files_mux(mux.channels, key, files, reporter);
      } else {
        reporter.status("parallel direct unavailable, using single direct connection (" + mux.fallback_reason + ")");
        send_files(mux.channels[0], key, files, reporter);
      }
    } else {
      send_files(direct_channel, key, files, reporter);
    }
    return 0;
  }

  throw KikoError("route selection did not produce a usable channel");
}

int run_recv(const RecvConfig& config, ProgressReporter& reporter) {
  auto listener = TcpListener::bind(config.listen);
  auto local_listen = listener.local_endpoint();
  reporter.status("listening for direct peer on " + local_listen.to_string());

  const auto external_relay = relay_with_manual_ip(config.relay, config.manual_ip);
  if (config.debug_route) {
    emit_debug_route(external_relay, config.proxy, config.relay_pass, config.bind_interface, config.avoid_vpn,
                     config.udp_probe, config.no_direct, config.only_local, reporter);
  }
  const auto connect_options =
      make_connect_options(external_relay, config.proxy, config.bind_interface, config.avoid_vpn, reporter);
  std::vector<Endpoint> relay_targets;
  std::vector<Endpoint> lan_extra;
  if (config.lan_discover && !config.disable_local && !config.manual_ip) {
    auto discovered = lan_discover(std::chrono::milliseconds(200));
    lan_extra = discovered;
    if (config.only_local) {
      relay_targets = std::move(discovered);
    } else {
      for (const auto& ep : discovered) push_unique_endpoint(relay_targets, ep);
      push_unique_endpoint(relay_targets, external_relay);
    }
  } else if (config.only_local) {
    throw KikoError("only-local mode requires LAN relay discovery");
  } else {
    relay_targets.push_back(external_relay);
  }
  if (relay_targets.empty()) throw KikoError("no relay endpoints to try");

  std::optional<StunProbeResult> stun_early;
  std::future<StunProbeResult> stun_future;
  const bool run_stun_probe = should_run_stun_probe(config.udp_probe, config.ai_route, config.ai_route_plan_only);
  if (run_stun_probe) {
    if (!config.udp_probe) reporter.status("ai route: running STUN NAT probe");
    stun_future = std::async(std::launch::async, [] { return probe_stun_nat(std::chrono::milliseconds(800)); });
    if (stun_future.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready) {
      stun_early = stun_future.get();
    }
  }

  auto local_addrs = local_lan_candidate_addresses();
  auto advertised_listen = local_listen;
  apply_manual_ip(local_addrs, advertised_listen, config.manual_ip);
  Message hello{"hello",
                {{"room", room_token(config.code)},
                 {"role", "receiver"},
                 {"listen_host", advertised_listen.host},
                 {"listen_port", std::to_string(local_listen.port)},
                 {"no_direct", config.no_direct ? "1" : "0"},
                 {"local_candidates", join_csv(local_addrs)}}};
  if (stun_early && stun_early->ok) hello.fields["stun_nat"] = stun_nat_class_name(stun_early->nat_class);

  auto race_entries = relay_race_entries_for_recv(relay_targets, external_relay);
  auto relay_probes = probe_and_sort_relay_race_entries(race_entries, external_relay, connect_options);

  auto peer_result = race_until_peer(race_entries, hello, std::chrono::seconds(30), connect_options, config.relay_pass);
  if (!peer_result) throw KikoError("failed to connect any relay or rendezvous peer");
  auto relay = std::move(peer_result->socket);
  auto peer = std::move(peer_result->peer);
  const auto active_relay = peer_result->relay;

  reporter.transfer_overview(peer.get_u64("file_count", 0), peer.get_u64("total_size", 0));
  std::filesystem::create_directories(config.output_dir);

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
  if (auto profile = load_profile(network_fingerprint())) apply_profile_to_snapshot(*profile, snapshot);
  snapshot.file_count = static_cast<std::size_t>(peer.get_u64("file_count", 0));
  snapshot.total_bytes = peer.get_u64("total_size", 0);
  snapshot.self_nat = self_nat.type;
  snapshot.peer_nat = peer_nat.type;
  snapshot.relays = std::move(relay_probes);
  if (stun && stun->ok) snapshot.stun_nat = stun->nat_class;

  // Sender hello conn_count is authoritative for mux width.
  const auto peer_conn_count = peer.get_u64("conn_count", 1);
  if (peer_conn_count > static_cast<std::uint64_t>(kMaxMuxConnections)) {
    throw KikoError("peer requested too many mux connections");
  }
  const int mux_connections = normalize_connection_count(static_cast<int>(peer_conn_count));
  RoutePlan route_plan = build_route_plan(config.no_direct, snapshot, stun, mux_connections);
  if (config.ai_route || config.ai_route_plan_only) {
    const auto ai_plan = resolve_route_plan(config.no_direct, snapshot, stun, mux_connections, config.ai_route,
                                            config.ai_route_plan_only, false, reporter);
    if (config.ai_route && !config.ai_route_plan_only) {
      route_plan = merge_route_plan_hint(route_plan, ai_plan);
    }
  }
  apply_peer_direct_policy(route_plan, peer);
  reporter.status("route plan: " + route_plan.reason + (route_plan.skip_direct ? " (skip direct)" : ""));
  const int connections = mux_connections;

  auto run_relay_recv = [&](TcpSocket relay_channel, bool allow_lan_upgrade,
                            const std::optional<PunchStats>& profile_stats = std::nullopt) -> int {
    const bool peer_no_direct = peer.get("peer_no_direct") == "1";
    if (allow_lan_upgrade && config.lan_discover && !config.no_direct && !peer_no_direct) {
      relay_channel =
          resolve_relay_channel(Role::Receiver, std::move(relay_channel), listener, local_listen.port, local_addrs,
                                config.no_direct);
    }
    receive_files_over_relay(std::move(relay_channel), active_relay, config.code, connections, connect_options,
                             config.relay_pass, config.output_dir, reporter);
    if (profile_stats) save_profile_success(network_fingerprint(), "relay", *profile_stats);
    else save_profile_success(network_fingerprint(), "relay");
    return 0;
  };

  AdaptivePuncher puncher;
  auto direct =
      attempt_direct(Role::Receiver, listener, peer, lan_extra, puncher, self_nat, peer_nat, route_plan,
                     room_token(config.code), connect_options, &reporter);
  emit_punch_report(puncher, reporter);
  auto selected_route =
      select_transfer_route(std::move(relay), std::move(direct), puncher, route_plan, reporter, kRelayRouteConfirmTimeout);
  if (selected_route.path == RoutePath::Relay) {
    if (selected_route.explain_direct_failure) {
      snapshot.punch = selected_route.punch_stats;
      explain_direct_failure(snapshot, route_plan, config.ai_route, reporter);
    }
    return run_relay_recv(std::move(selected_route.relay), selected_route.allow_lan_upgrade, selected_route.punch_stats);
  }

  if (selected_route.direct) {
    auto direct_channel = std::move(*selected_route.direct);
    auto key = perform_handshake(direct_channel, Role::Receiver, config.code);
    reporter.handshake_ok();
    save_profile_success(network_fingerprint(), "direct", selected_route.punch_stats);
    if (connections > 1) {
      auto mux = negotiate_direct_mux_channels(std::move(direct_channel), Role::Receiver, listener, peer, connections,
                                               room_token(config.code), connect_options, kDirectMuxSetupTimeout);
      if (mux.mux_enabled) {
        reporter.status("opening " + std::to_string(connections) + " parallel direct connections");
        receive_files_mux(mux.channels, key, config.output_dir, reporter);
      } else {
        reporter.status("parallel direct unavailable, using single direct connection (" + mux.fallback_reason + ")");
        receive_files(mux.channels[0], key, config.output_dir, reporter);
      }
    } else {
      receive_files(direct_channel, key, config.output_dir, reporter);
    }
    return 0;
  }

  throw KikoError("route selection did not produce a usable channel");
}

}  // namespace kiko
