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

std::string describe_route_plan_for_transfer(const RoutePlan& plan) {
  std::string line = plan.reason;
  if (plan.skip_direct) return line + " (skip direct)";
  line += " direct_window=" + std::to_string(plan.direct_timeout.count()) + "ms";
  line += " direct_connect=" + std::to_string(plan.direct_connect.count()) + "ms";
  line += " same_port=" + std::to_string(plan.same_port_timeout.count()) + "ms/" +
          std::to_string(plan.same_port_connect.count()) + "ms";
  if (plan.udp_punch_enabled) line += " udp-assist";
  return line;
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

template <typename Fn>
int run_with_auto_reconnect(int max_attempts, std::chrono::milliseconds delay, ProgressReporter& reporter,
                            const std::shared_ptr<TransferCancellation>& cancellation, Fn&& fn) {
  for (int attempt = 1;; ++attempt) {
    try {
      if (cancellation && cancellation->requested()) throw KikoError("transfer canceled");
      return fn();
    } catch (const std::exception& error) {
      if (cancellation && cancellation->requested()) throw KikoError("transfer canceled");
      if (attempt >= max_attempts || !is_retryable_transfer_error(error)) throw;
      reporter.transfer_retry(attempt + 1, max_attempts, error.what());
      reporter.transfer_retry_delay(attempt + 1, max_attempts, delay);
      auto remaining = delay;
      while (remaining.count() > 0) {
        if (cancellation && cancellation->requested()) throw KikoError("transfer canceled");
        const auto slice = std::min<std::chrono::milliseconds>(remaining, std::chrono::milliseconds(50));
        std::this_thread::sleep_for(slice);
        remaining -= slice;
      }
    }
  }
}

void throw_if_cancelled(const std::shared_ptr<TransferCancellation>& cancellation) {
  if (cancellation && cancellation->requested()) throw KikoError("transfer canceled");
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
  const auto external_relay = relay_with_manual_ip(config.relay, config.manual_ip);
  if (config.debug_route) {
    emit_debug_route(external_relay, config.proxy, config.relay_pass, config.bind_interface, config.avoid_vpn,
                     config.udp_probe, config.no_direct, config.only_local, reporter);
  }

  const bool run_stun_probe = should_run_stun_probe(config.udp_probe, config.ai_route, config.ai_route_plan_only);
  if (run_stun_probe && !config.udp_probe) reporter.status("ai route: running STUN NAT probe");
  auto route_config =
      make_route_config(config, Role::Sender, config.code, config.show_qrcode, run_stun_probe,
                        "failed to connect relay or rendezvous peer");
  PeerRouteSession route(std::move(route_config), reporter);
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
        resolve_route_plan(config.no_direct, pre_snapshot, route.early_stun(), connections, config.ai_route,
                           config.ai_route_plan_only, config.ai_route_connectivity_only, reporter);
    if (config.ai_route && !config.ai_route_plan_only) {
      if (!config.ai_route_connectivity_only) connections = normalize_connection_count(pre_plan.connections);
      pre_peer_ai_plan = pre_plan;
      if (!pre_plan.relay_order.empty()) route.apply_relay_order(pre_plan.relay_order);
    }
  }

  route.rendezvous({{"conn_index", "0"},
                    {"conn_count", std::to_string(connections)},
                    {"file_count", std::to_string(files.size())},
                    {"total_size", std::to_string(total_size)}});

  auto snapshot = route.connectivity_snapshot(total_size);
  fill_transfer_snapshot(snapshot, files, connections_hint);
  auto route_plan = build_route_plan(config.no_direct, snapshot, route.stun(), connections);
  if (pre_peer_ai_plan && config.ai_route && !config.ai_route_plan_only) {
    route_plan = merge_route_plan_hint(route_plan, *pre_peer_ai_plan);
    connections = normalize_connection_count(route_plan.connections);
  }
  route_plan = route.apply_peer_policy(std::move(route_plan));
  reporter.status("route plan: " + describe_route_plan_for_transfer(route_plan));

  auto established = route.establish(route_plan, connections);
  if (established.path == RoutePath::Relay && established.explain_direct_failure) {
    snapshot.punch = established.punch_stats;
    explain_direct_failure(snapshot, route_plan, config.ai_route, reporter);
  }
  send_established_route(established, files, reporter);
  route.record_success(established);
  return 0;
}

int run_recv_once(const RecvConfig& config, ProgressReporter& reporter) {
  throw_if_cancelled(config.cancellation);
  const auto external_relay = relay_with_manual_ip(config.relay, config.manual_ip);
  if (config.debug_route) {
    emit_debug_route(external_relay, config.proxy, config.relay_pass, config.bind_interface, config.avoid_vpn,
                     config.udp_probe, config.no_direct, config.only_local, reporter);
  }

  const bool run_stun_probe = should_run_stun_probe(config.udp_probe, config.ai_route, config.ai_route_plan_only);
  if (run_stun_probe && !config.udp_probe) reporter.status("ai route: running STUN NAT probe");
  auto route_config =
      make_route_config(config, Role::Receiver, config.code, false, run_stun_probe,
                        "failed to connect any relay or rendezvous peer");
  PeerRouteSession route(std::move(route_config), reporter);
  route.rendezvous();

  const auto& peer = route.peer();
  const auto file_count = static_cast<std::size_t>(peer.get_u64("file_count", 0));
  const auto total_size = peer.get_u64("total_size", 0);
  reporter.transfer_overview(file_count, total_size);
  std::filesystem::create_directories(config.output_dir);

  auto snapshot = route.connectivity_snapshot(total_size);
  snapshot.file_count = file_count;

  const auto peer_connection_count = peer.get_u64("conn_count", 1);
  if (peer_connection_count > static_cast<std::uint64_t>(kMaxMuxConnections)) {
    throw KikoError("peer requested too many mux connections");
  }
  const int connections = normalize_connection_count(static_cast<int>(peer_connection_count));
  auto route_plan = build_route_plan(config.no_direct, snapshot, route.stun(), connections);
  if (config.ai_route || config.ai_route_plan_only) {
    const auto ai_plan = resolve_route_plan(config.no_direct, snapshot, route.stun(), connections, config.ai_route,
                                            config.ai_route_plan_only, false, reporter);
    if (config.ai_route && !config.ai_route_plan_only) route_plan = merge_route_plan_hint(route_plan, ai_plan);
  }
  route_plan = route.apply_peer_policy(std::move(route_plan));
  reporter.status("route plan: " + describe_route_plan_for_transfer(route_plan));

  auto established = route.establish(route_plan, connections);
  if (established.path == RoutePath::Relay && established.explain_direct_failure) {
    snapshot.punch = established.punch_stats;
    explain_direct_failure(snapshot, route_plan, config.ai_route, reporter);
  }
  receive_established_route(established, config.output_dir, reporter, config.conflict_policy);
  route.record_success(established);
  return 0;
}

int run_send(const SendConfig& config, ProgressReporter& reporter) {
  SendConfig attempt_config = config;
  if (attempt_config.code.empty()) attempt_config.code = random_code(3);
  else attempt_config.code = normalize_pairing_code(attempt_config.code);
  if (auto error = validate_pairing_code_format(attempt_config.code, true)) throw KikoError(*error);
  return run_with_auto_reconnect(
      total_transfer_attempts(attempt_config.auto_reconnect, attempt_config.reconnect_attempts),
      attempt_config.reconnect_delay, reporter, attempt_config.cancellation,
      [&]() { return run_send_once(attempt_config, reporter); });
}

int run_recv(const RecvConfig& config, ProgressReporter& reporter) {
  RecvConfig attempt_config = config;
  attempt_config.code = normalize_pairing_code(attempt_config.code);
  if (auto error = validate_pairing_code_format(attempt_config.code, true)) throw KikoError(*error);
  return run_with_auto_reconnect(total_transfer_attempts(attempt_config.auto_reconnect, attempt_config.reconnect_attempts),
                                 attempt_config.reconnect_delay, reporter, attempt_config.cancellation,
                                 [&]() { return run_recv_once(attempt_config, reporter); });
}

}  // namespace kiko
