#include "transfer.hpp"

#include "connect/peer_route_session.hpp"
#include "connect/rendezvous_session.hpp"
#include "connect/route_planner.hpp"
#include "core/cancellation.hpp"
#include "diagnostics/doctor.hpp"
#include "diagnostics/network_probe.hpp"
#include "transfer_heuristics.hpp"
#include "transfer_retry.hpp"
#include "transfer_stream.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>

namespace kiko {

using namespace detail;

namespace {

int normalize_connection_count(int connections) {
  if (connections < 1) return 1;
  if (connections > kMaxMuxConnections) {
    throw KikoError("connection count exceeds maximum " + std::to_string(kMaxMuxConnections));
  }
  return connections;
}

void fill_transfer_snapshot(ConnectivitySnapshot& snapshot, const std::vector<FileEntry>& files, int connections_hint) {
  const auto stats = transfer_payload_stats(files);
  snapshot.file_count = files.size();
  snapshot.largest_file_bytes = stats.largest_file_bytes;
  snapshot.compressible_ratio = stats.compressible_ratio;
  snapshot.connections_hint = connections_hint;
}

void emit_debug_route(const PeerConnectionOptions& config, ProgressReporter& reporter) {
  DoctorOptions options;
  options.relay = relay_with_manual_ip(config.relay, config.manual_ip);
  options.proxy = config.proxy;
  options.relay_pass = config.relay_pass;
  options.bind_interface = config.bind_interface;
  options.avoid_vpn = config.avoid_vpn;
  options.udp_probe = config.udp_probe;
  options.no_direct = config.no_direct;
  options.only_local = config.only_local;
  const auto report = run_doctor(options);
  for (const auto& line : doctor_debug_lines(report)) reporter.status(line);
}

PeerRouteSessionConfig make_route_config(const PeerConnectionOptions& connection, Role role, const std::string& code,
                                         bool show_qrcode, bool run_stun_probe, std::string failure_message) {
  PeerRouteSessionConfig config;
  config.connection = connection;
  config.role = role;
  config.code = code;
  config.show_qrcode = show_qrcode;
  config.run_stun_probe = run_stun_probe;
  config.use_profile = true;
  config.failure_message = std::move(failure_message);
  return config;
}

template <typename Config, typename Fn>
int run_with_auto_reconnect(Config config, bool generate_code, ProgressReporter& reporter, Fn run_once) {
  if (generate_code && config.code.empty()) config.code = random_code(3);
  else config.code = normalize_pairing_code(config.code);
  if (auto error = validate_pairing_code_format(config.code, true)) throw KikoError(*error);

  const int max_attempts = total_transfer_attempts(config.auto_reconnect, config.reconnect_attempts);
  for (int attempt = 1;; ++attempt) {
    try {
      throw_if_cancelled(config.cancellation);
      return run_once(config, reporter);
    } catch (const std::exception& error) {
      throw_if_cancelled(config.cancellation);
      if (attempt >= max_attempts || !is_retryable_transfer_error(error)) throw;
      reporter.transfer_retry(attempt + 1, max_attempts, error.what());
      reporter.transfer_retry_delay(attempt + 1, max_attempts, config.reconnect_delay);
      auto remaining = config.reconnect_delay;
      while (remaining.count() > 0) {
        throw_if_cancelled(config.cancellation);
        const auto slice = std::min<std::chrono::milliseconds>(remaining, std::chrono::milliseconds(50));
        std::this_thread::sleep_for(slice);
        remaining -= slice;
      }
    }
  }
}

void send_established_route(EstablishedPeerRoute& established, const std::vector<FileEntry>& files,
                            ProgressReporter& reporter) {
  if (established.channels.empty()) throw KikoError("peer route did not establish a transfer channel");
  if (established.mux_enabled) {
    send_files_mux(established.channels, established.key, files, reporter);
  } else {
    send_files(established.channels.front(), established.key, files, reporter);
  }
}

void receive_established_route(EstablishedPeerRoute& established, const std::filesystem::path& output_dir,
                               ProgressReporter& reporter, ConflictPolicy conflict_policy) {
  if (established.channels.empty()) throw KikoError("peer route did not establish a transfer channel");
  if (established.mux_enabled) {
    receive_files_mux(established.channels, established.key, output_dir, reporter, conflict_policy);
  } else {
    receive_files(established.channels.front(), established.key, output_dir, reporter, conflict_policy);
  }
}

EstablishedPeerRoute establish_transfer_route(PeerRouteSession& route, RoutePlan plan, int connections,
                                               ConnectivitySnapshot& snapshot, bool ai_route,
                                               ProgressReporter& reporter) {
  auto established = route.establish(plan, connections);
  if (established.path == RoutePath::Relay && established.explain_direct_failure) {
    snapshot.punch = established.punch_stats;
    explain_direct_failure(snapshot, established.route_plan, ai_route, reporter);
  }
  return established;
}

}  // namespace

int run_send_once(const SendConfig& config, ProgressReporter& reporter) {
  throw_if_cancelled(config.cancellation);
  CollectOptions collect_options;
  collect_options.use_gitignore = config.use_gitignore;
  collect_options.symlink_mode = config.symlink_mode;
  auto files = collect_files(config.file, collect_options);
  std::uint64_t total_size = 0;
  for (const auto& entry : files) total_size += entry.size;

  int connections = normalize_connection_count(config.connections);
  if (config.debug_route) emit_debug_route(config, reporter);

  const bool run_stun_probe = should_run_stun_probe(config.udp_probe, config.ai_route, config.ai_route_plan_only);
  if (run_stun_probe && !config.udp_probe) reporter.status("ai route: running STUN NAT probe");
  PeerRouteSession route(make_route_config(config, Role::Sender, config.code, config.show_qrcode, run_stun_probe,
                                           "failed to connect relay or rendezvous peer"),
                         reporter);
  reporter.transfer_overview(files.size(), total_size);

  if (config.auto_connections) {
    const auto rtt = route.probe_external_relay_rtt();
    connections = normalize_connection_count(recommend_connections(rtt, total_size));
    reporter.status("auto connections: " + std::to_string(connections) +
                    (rtt >= 0 ? " (relay rtt " + std::to_string(rtt) + "ms)" : ""));
    if (config.ai_route && !config.ai_route_connectivity_only) {
      reporter.status("auto connections is a hint; --ai-route may override unless --ai-route-connectivity-only");
    }
  }

  const int connections_hint = connections;
  std::optional<RoutePlan> pre_peer_ai_plan;
  if (config.ai_route || config.ai_route_plan_only) {
    auto pre_snapshot = route.pre_rendezvous_snapshot(total_size);
    fill_transfer_snapshot(pre_snapshot, files, connections_hint);
    const auto pre_plan =
        resolve_route_plan(config.no_direct, pre_snapshot, route.stun_probe(), connections, config.ai_route,
                           config.ai_route_plan_only, config.ai_route_connectivity_only, reporter);
    if (config.ai_route && !config.ai_route_plan_only) {
      if (!config.ai_route_connectivity_only) connections = normalize_connection_count(pre_plan.connections);
      pre_peer_ai_plan = pre_plan;
      if (!pre_plan.relay_order.empty()) route.apply_relay_order(pre_plan.relay_order);
    }
  }

  RelayHello hello;
  hello.conn_count = static_cast<std::uint64_t>(connections);
  hello.file_count = files.size();
  hello.total_size = total_size;
  route.rendezvous(std::move(hello));

  auto snapshot = route.connectivity_snapshot(total_size);
  fill_transfer_snapshot(snapshot, files, connections_hint);
  auto route_plan = build_route_plan(config.no_direct, snapshot, route.stun_probe(), connections);
  if (pre_peer_ai_plan && config.ai_route && !config.ai_route_plan_only) {
    route_plan = merge_route_plan_hint(route_plan, *pre_peer_ai_plan);
    connections = normalize_connection_count(route_plan.connections);
  }
  auto established =
      establish_transfer_route(route, std::move(route_plan), connections, snapshot, config.ai_route, reporter);
  send_established_route(established, files, reporter);
  route.record_success(established);
  return 0;
}

int run_recv_once(const RecvConfig& config, ProgressReporter& reporter) {
  throw_if_cancelled(config.cancellation);
  if (config.debug_route) emit_debug_route(config, reporter);

  const bool run_stun_probe = should_run_stun_probe(config.udp_probe, config.ai_route, config.ai_route_plan_only);
  if (run_stun_probe && !config.udp_probe) reporter.status("ai route: running STUN NAT probe");
  PeerRouteSession route(make_route_config(config, Role::Receiver, config.code, false, run_stun_probe,
                                           "failed to connect any relay or rendezvous peer"),
                         reporter);
  route.rendezvous();

  const auto& peer = route.peer();
  const auto file_count = static_cast<std::size_t>(peer.file_count);
  const auto total_size = peer.total_size;
  reporter.transfer_overview(file_count, total_size);
  std::filesystem::create_directories(config.output_dir);

  auto snapshot = route.connectivity_snapshot(total_size);
  snapshot.file_count = file_count;

  const auto peer_connection_count = peer.conn_count;
  if (peer_connection_count > static_cast<std::uint64_t>(kMaxMuxConnections)) {
    throw KikoError("peer requested too many mux connections");
  }
  const int connections = normalize_connection_count(static_cast<int>(peer_connection_count));
  auto route_plan = resolve_route_plan(config.no_direct, snapshot, route.stun_probe(), connections, config.ai_route,
                                       config.ai_route_plan_only, false, reporter);
  auto established =
      establish_transfer_route(route, std::move(route_plan), connections, snapshot, config.ai_route, reporter);
  receive_established_route(established, config.output_dir, reporter, config.conflict_policy);
  route.record_success(established);
  return 0;
}

int run_send(const SendConfig& config, ProgressReporter& reporter) {
  return run_with_auto_reconnect(config, true, reporter, run_send_once);
}

int run_recv(const RecvConfig& config, ProgressReporter& reporter) {
  return run_with_auto_reconnect(config, false, reporter, run_recv_once);
}

}  // namespace kiko
