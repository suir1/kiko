#include "connectivity.hpp"

#include "adaptive.hpp"
#include "protocol.hpp"
#include "socket.hpp"

#include <algorithm>
#include <atomic>
#include <optional>
#include <thread>

namespace kiko {
namespace {

constexpr auto kDirectPreflightTimeout = std::chrono::milliseconds(1500);
constexpr auto kRelayStandbyDirectWindow = std::chrono::milliseconds(500);
constexpr auto kRelayStandbyConnectWindow = std::chrono::milliseconds(220);

bool direct_cancelled(const std::atomic_bool* cancel) { return cancel && cancel->load(); }

}  // namespace

ConnectivitySnapshot build_pre_rendezvous_snapshot(bool no_direct, bool only_local, std::size_t lan_discovered_count,
                                                   std::uint64_t total_bytes) {
  ConnectivitySnapshot s;
  s.no_direct_config = no_direct;
  s.only_local = only_local;
  s.lan_discovered_count = lan_discovered_count;
  s.total_bytes = total_bytes;
  s.vpn_detected = detect_vpn_interfaces();
  s.lan_candidates = local_lan_candidate_addresses();
  return s;
}

RoutePlan RuleScheduler::plan(const ConnectivitySnapshot& snapshot, const std::optional<StunProbeResult>& stun,
                              bool force_no_direct, int default_connections) const {
  RoutePlan plan;
  plan.connections = default_connections;
  plan.reason = "default";

  if (force_no_direct || snapshot.no_direct_config) {
    plan.skip_direct = true;
    plan.direct_timeout = std::chrono::milliseconds(0);
    plan.reason = "no_direct";
    return plan;
  }

  if (snapshot.only_local) {
    plan.reason = "only_local";
  }

  if (snapshot.vpn_detected && snapshot.lan_discovered_count > 0) {
    plan.direct_timeout = std::chrono::milliseconds(1000);
    plan.direct_connect = std::chrono::milliseconds(250);
    plan.reason = "vpn_lan_short_direct";
  }

  if (stun && stun->ok) {
    if (stun->nat_class == StunNatClass::Symmetric) {
      plan.direct_timeout = kRelayStandbyDirectWindow;
      plan.direct_connect = kRelayStandbyConnectWindow;
      plan.reason = "stun_symmetric_short_direct";
    } else if (stun->nat_class == StunNatClass::Open) {
      plan.direct_timeout = std::chrono::milliseconds(3500);
      plan.reason = "stun_open";
    }
  }

  if (snapshot.self_nat == NatType::BehindNat && snapshot.peer_nat == NatType::BehindNat) {
    if (plan.direct_timeout > kRelayStandbyDirectWindow) {
      plan.direct_timeout = kRelayStandbyDirectWindow;
    }
    plan.direct_connect = kRelayStandbyConnectWindow;
    if (plan.reason == "default") plan.reason = "double_nat_short_punch";
  }

  if (!plan.skip_direct) {
    const StunNatClass nat = stun && stun->ok ? stun->nat_class : snapshot.stun_nat;
    if (nat == StunNatClass::Cone) {
      // UDP is currently a probe signal only. Do not promise UDP-assisted TCP
      // punching until we can coordinate a real same-path TCP punch strategy.
      if (plan.reason == "default") plan.reason = "stun_cone_direct_probe";
    }
  }

  return plan;
}

void apply_route_plan_to_adaptive(const RoutePlan& plan, Role role, AdaptivePuncher& puncher,
                                  const std::vector<DirectCandidate>& candidates, const NatProfile& self,
                                  const NatProfile& peer, PunchPlan& out) {
  auto ordered = candidates;
  apply_direct_candidate_kind_order(ordered, plan.direct_candidate_order);
  out = puncher.plan(role, ordered, self, peer);
  if (plan.direct_timeout.count() > 0 && plan.direct_timeout < out.total_timeout) {
    out.total_timeout = plan.direct_timeout;
  }
  if (plan.direct_connect.count() > 0) {
    out.connect_timeout = plan.direct_connect;
  }
  tune_direct_candidate_timeouts(out);
}

namespace {

std::chrono::milliseconds remaining_until(std::chrono::steady_clock::time_point deadline) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
}

bool direct_ack_ok(const Message& message, const std::string& room) {
  return message.type == "direct_ack" && message.get("room") == room;
}

bool direct_hello_ok(const Message& message, const std::string& room, Role expected_role) {
  return message.type == "direct_hello" && message.get("room") == room && message.get("role") == role_name(expected_role);
}

Role peer_role(Role role) { return role == Role::Sender ? Role::Receiver : Role::Sender; }

std::chrono::milliseconds candidate_connect_timeout(const DirectCandidate& candidate,
                                                    std::chrono::milliseconds fallback_timeout,
                                                    std::chrono::steady_clock::time_point deadline) {
  auto timeout = candidate.connect_timeout.count() > 0 ? candidate.connect_timeout : fallback_timeout;
  const auto remaining = remaining_until(deadline);
  if (remaining.count() <= 0) return std::chrono::milliseconds(0);
  return std::min(timeout, remaining);
}

std::optional<std::uint64_t> parse_punch_token_ms(const std::string& token) {
  if (token.empty()) return std::nullopt;
  return parse_u64_strict(token);
}

void wait_until_punch_time(const std::string& punch_token, std::chrono::steady_clock::time_point deadline,
                           const std::atomic_bool* cancel) {
  std::optional<std::uint64_t> punch_at;
  try {
    punch_at = parse_punch_token_ms(punch_token);
  } catch (const KikoError&) {
    return;
  }
  if (!punch_at) return;

  while (!direct_cancelled(cancel) && std::chrono::steady_clock::now() < deadline) {
    const auto now = now_ms();
    if (now >= *punch_at) return;
    if (*punch_at - now > 2000) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

bool complete_direct_dial_preflight(TcpSocket& socket, Role role, const std::string& room) {
  send_message(socket, Message{"direct_hello", {{"room", room}, {"role", role_name(role)}}});
  auto response = recv_message_timeout(socket, kDirectPreflightTimeout);
  if (!response) return false;
  if (direct_ack_ok(*response, room)) return true;

  if (!direct_hello_ok(*response, room, peer_role(role))) return false;

  send_message(socket, Message{"direct_ack", {{"room", room}}});
  auto peer_ack = recv_message_timeout(socket, kDirectPreflightTimeout);
  return peer_ack && direct_ack_ok(*peer_ack, room);
}

std::optional<TcpSocket> dial_direct_candidate(const DirectCandidate& candidate, std::chrono::milliseconds connect_timeout,
                                               Role role, const std::string& room, AdaptivePuncher& puncher,
                                               const ConnectOptions& connect_options, const std::string& phase) {
  const auto start = now_ms();
  auto socket = connect_tcp(candidate.endpoint, connect_timeout, connect_options);
  if (!socket.valid()) {
    puncher.observe(PunchObservation{phase, candidate.endpoint.to_string(), candidate.kind, candidate.priority, false,
                                     now_ms() - start, "connect_failed"});
    return std::nullopt;
  }

  try {
    const bool ok = complete_direct_dial_preflight(socket, role, room);
    puncher.observe(PunchObservation{phase, candidate.endpoint.to_string(), candidate.kind, candidate.priority, ok,
                                     now_ms() - start, ok ? "" : "direct_ack_failed"});
    if (ok) return socket;
  } catch (const std::exception& e) {
    puncher.observe(PunchObservation{phase, candidate.endpoint.to_string(), candidate.kind, candidate.priority, false,
                                     now_ms() - start, e.what()});
  }
  socket.close();
  return std::nullopt;
}

std::vector<DirectCandidate> public_candidates_only(const std::vector<DirectCandidate>& candidates) {
  std::vector<DirectCandidate> out;
  for (const auto& candidate : candidates) {
    if (candidate.kind == "public") out.push_back(candidate);
  }
  return out;
}

std::optional<TcpSocket> dial_direct_candidate_same_port(const DirectCandidate& candidate,
                                                         std::chrono::milliseconds connect_timeout, Role role,
                                                         const std::string& room, AdaptivePuncher& puncher,
                                                         const ConnectOptions& connect_options,
                                                         std::uint16_t local_port, const std::string& phase) {
  if (candidate.kind != "public" || local_port == 0 || connect_options.proxy) return std::nullopt;

  auto same_port_options = connect_options;
  same_port_options.local_bind = Endpoint{"", local_port};

  auto same_port_candidate = candidate;
  same_port_candidate.kind = "public-same-port";
  add_direct_candidate_reason(same_port_candidate, "same_port_probe");
  return dial_direct_candidate(same_port_candidate, connect_timeout, role, room, puncher, same_port_options, phase);
}

std::optional<TcpSocket> accept_direct_candidate(TcpListener& listener, std::chrono::milliseconds timeout,
                                                 const std::string& room, Role expected_role,
                                                 AdaptivePuncher& puncher, const std::string& phase) {
  auto accepted = listener.accept(timeout);
  if (!accepted.valid()) return std::nullopt;

  try {
    auto hello = recv_message_timeout(accepted, kDirectPreflightTimeout);
    const bool ok = hello && direct_hello_ok(*hello, room, expected_role);
    if (ok) {
      send_message(accepted, Message{"direct_ack", {{"room", room}}});
      puncher.observe(PunchObservation{phase, "listener", "accept", 0, true, 0, ""});
      return accepted;
    }
    puncher.observe(PunchObservation{phase, "listener", "accept", 0, false, 0, "direct_hello_mismatch"});
  } catch (const std::exception& e) {
    puncher.observe(PunchObservation{phase, "listener", "accept", 0, false, 0, e.what()});
  }
  accepted.close();
  return std::nullopt;
}

std::optional<TcpSocket> try_direct_phase(Role self_role, Role active_role, TcpListener& listener, const PunchPlan& plan,
                                          AdaptivePuncher& puncher, const std::string& room,
                                          std::chrono::steady_clock::time_point phase_deadline,
                                          const ConnectOptions& connect_options, const std::atomic_bool* cancel) {
  std::uint16_t local_port = 0;
  try {
    local_port = listener.local_endpoint().port;
  } catch (const std::exception&) {
  }

  while (!direct_cancelled(cancel) && std::chrono::steady_clock::now() < phase_deadline) {
    const std::string phase = active_role == Role::Receiver ? "receiver-active" : "sender-active";
    if (self_role == active_role) {
      for (const auto& candidate : plan.candidates) {
        if (direct_cancelled(cancel)) break;
        if (std::chrono::steady_clock::now() >= phase_deadline) break;
        const auto connect_timeout = candidate_connect_timeout(candidate, plan.connect_timeout, phase_deadline);
        if (connect_timeout.count() <= 0) break;
        if (auto socket = dial_direct_candidate_same_port(candidate, connect_timeout, self_role, room, puncher,
                                                         connect_options, local_port, phase + "-same-port")) {
          return socket;
        }
        const auto remaining_timeout = candidate_connect_timeout(candidate, plan.connect_timeout, phase_deadline);
        if (remaining_timeout.count() <= 0) break;
        if (auto socket = dial_direct_candidate(candidate, remaining_timeout, self_role, room, puncher,
                                               connect_options, phase)) {
          return socket;
        }
      }
    }

    const auto remaining = remaining_until(phase_deadline);
    if (remaining.count() <= 0) break;
    const auto accept_timeout = std::min<std::chrono::milliseconds>(remaining, std::chrono::milliseconds(80));
    if (auto accepted = accept_direct_candidate(listener, accept_timeout, room, active_role, puncher, phase + "-accept")) {
      return accepted;
    }

    if (!direct_cancelled(cancel)) std::this_thread::sleep_for(std::min(plan.retry_delay, std::chrono::milliseconds(50)));
  }
  return std::nullopt;
}

std::optional<TcpSocket> try_synchronized_same_port_phase(Role role, TcpListener& listener, const PunchPlan& plan,
                                                          AdaptivePuncher& puncher, const std::string& room,
                                                          std::chrono::steady_clock::time_point phase_deadline,
                                                          const ConnectOptions& connect_options,
                                                          const std::string& punch_token,
                                                          const std::atomic_bool* cancel) {
  if (connect_options.proxy) return std::nullopt;
  auto candidates = public_candidates_only(plan.candidates);
  if (candidates.empty()) return std::nullopt;

  std::uint16_t local_port = 0;
  try {
    local_port = listener.local_endpoint().port;
  } catch (const std::exception&) {
  }
  if (local_port == 0) return std::nullopt;

  wait_until_punch_time(punch_token, phase_deadline, cancel);
  while (!direct_cancelled(cancel) && std::chrono::steady_clock::now() < phase_deadline) {
    for (const auto& candidate : candidates) {
      if (direct_cancelled(cancel)) break;
      const auto timeout = candidate_connect_timeout(candidate, plan.connect_timeout, phase_deadline);
      if (timeout.count() <= 0) break;
      if (auto socket = dial_direct_candidate_same_port(candidate, timeout, role, room, puncher, connect_options,
                                                       local_port, "sync-same-port")) {
        return socket;
      }
    }
    const auto remaining = remaining_until(phase_deadline);
    if (remaining.count() <= 0) break;
    const auto accept_timeout = std::min<std::chrono::milliseconds>(remaining, std::chrono::milliseconds(40));
    if (auto accepted =
            accept_direct_candidate(listener, accept_timeout, room, peer_role(role), puncher, "sync-same-port-accept")) {
      return accepted;
    }
    if (!direct_cancelled(cancel)) std::this_thread::sleep_for(std::min(plan.retry_delay, std::chrono::milliseconds(40)));
  }
  return std::nullopt;
}

}  // namespace

std::optional<TcpSocket> try_direct_with_plan(Role role, TcpListener& listener, PunchPlan plan,
                                              AdaptivePuncher& puncher, const std::string& room,
                                              const ConnectOptions& connect_options, const std::string& punch_token,
                                              const std::atomic_bool* cancel) {
  if (plan.total_timeout.count() <= 0) return std::nullopt;
  if (direct_cancelled(cancel)) return std::nullopt;

  const auto start = std::chrono::steady_clock::now();
  const auto deadline = start + plan.total_timeout;
  const auto same_port_budget = std::min<std::chrono::milliseconds>(std::chrono::milliseconds(500), plan.total_timeout / 3);
  const auto same_port_deadline = std::min(deadline, start + same_port_budget);
  if (auto socket = try_synchronized_same_port_phase(role, listener, plan, puncher, room, same_port_deadline,
                                                     connect_options, punch_token, cancel)) {
    return socket;
  }
  if (direct_cancelled(cancel)) return std::nullopt;

  const auto first_phase_budget = std::max<std::chrono::milliseconds>(
      std::chrono::milliseconds(250),
      std::chrono::duration_cast<std::chrono::milliseconds>(plan.total_timeout * 2 / 3));
  const auto first_deadline = std::min(deadline, start + first_phase_budget);

  if (auto socket = try_direct_phase(role, Role::Receiver, listener, plan, puncher, room, first_deadline,
                                     connect_options, cancel)) {
    return socket;
  }
  if (direct_cancelled(cancel)) return std::nullopt;
  if (auto socket = try_direct_phase(role, Role::Sender, listener, plan, puncher, room, deadline, connect_options,
                                     cancel)) {
    return socket;
  }
  return std::nullopt;
}

PunchStats punch_stats_from(const AdaptivePuncher& puncher, bool direct_ok, bool attempted) {
  PunchStats stats;
  stats.attempted = attempted;
  stats.direct_ok = direct_ok;
  if (!attempted) return stats;

  for (const auto& observation : puncher.observations()) {
    if (observation.success) {
      if (stats.successful_candidate_kind.empty() || stats.successful_candidate_kind == "accept") {
        stats.successful_candidate_kind = observation.kind;
        stats.successful_candidate_priority = observation.priority;
        stats.successful_elapsed_ms = static_cast<std::int64_t>(observation.elapsed_ms);
      }
      continue;
    }
    const auto key = observation.error.empty() ? "unknown" : observation.error;
    ++stats.failures[key];
    if (!observation.kind.empty()) ++stats.candidate_failures_by_kind[observation.kind];
  }
  return stats;
}

void apply_direct_candidate_kind_order(std::vector<DirectCandidate>& candidates, const std::vector<std::string>& kind_order) {
  if (kind_order.empty() || candidates.empty()) return;

  auto rank = [&](const std::string& kind) {
    for (std::size_t i = 0; i < kind_order.size(); ++i) {
      if (kind_order[i] == kind) return static_cast<int>(i);
    }
    return static_cast<int>(kind_order.size());
  };

  for (auto& candidate : candidates) {
    const auto r = rank(candidate.kind);
    if (r < static_cast<int>(kind_order.size())) {
      candidate.priority += 1000 - r * 100;
      add_direct_candidate_reason(candidate, "route_order_hint");
    }
  }
}

}  // namespace kiko
