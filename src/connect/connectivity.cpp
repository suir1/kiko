#include "connectivity.hpp"

#include "core/adaptive.hpp"
#include "core/cancellation.hpp"
#include "core/protocol.hpp"
#include "core/socket.hpp"

#include <algorithm>
#include <atomic>
#include <optional>
#include <thread>

namespace kiko {
namespace {

constexpr auto kDirectPreflightTimeout = std::chrono::milliseconds(1500);
constexpr auto kSyncSamePortConnectWindow = std::chrono::milliseconds(160);

bool direct_cancelled(const std::atomic_bool* cancel) { return cancellation_requested(cancel); }

}  // namespace

ConnectivitySnapshot build_pre_rendezvous_snapshot(bool no_direct, bool only_local, std::size_t lan_discovered_count,
                                                   std::uint64_t total_bytes,
                                                   const NetworkInterfaceInventory& interfaces) {
  ConnectivitySnapshot s;
  s.no_direct_config = no_direct;
  s.only_local = only_local;
  s.lan_discovered_count = lan_discovered_count;
  s.total_bytes = total_bytes;
  s.vpn_detected = interfaces.vpn_detected();
  s.lan_candidates = interfaces.lan_candidate_addresses();
  s.self_global_ipv6_count = count_global_ipv6_addresses(s.lan_candidates);
  return s;
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

bool wait_until_punch_target(std::uint64_t target_ms, std::chrono::steady_clock::time_point deadline,
                             const std::atomic_bool* cancel) {
  while (!direct_cancelled(cancel) && std::chrono::steady_clock::now() < deadline) {
    const auto now = now_ms();
    if (now >= target_ms) return true;
    if (target_ms - now > 2000) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

std::uint64_t punch_target_with_offset(std::uint64_t punch_at_ms, int offset_ms) {
  if (offset_ms < 0) {
    const auto delta = static_cast<std::uint64_t>(-offset_ms);
    return punch_at_ms > delta ? punch_at_ms - delta : 0;
  }
  return punch_at_ms + static_cast<std::uint64_t>(offset_ms);
}

bool complete_direct_dial_preflight(TcpSocket& socket, Role role, const std::string& room,
                                    const std::atomic_bool* cancel) {
  if (direct_cancelled(cancel)) return false;
  send_message(socket, Message{"direct_hello", {{"room", room}, {"role", role_name(role)}}});
  auto response = recv_message_timeout(socket, kDirectPreflightTimeout, cancel);
  if (direct_cancelled(cancel)) return false;
  if (!response) return false;
  if (direct_ack_ok(*response, room)) return true;

  if (!direct_hello_ok(*response, room, peer_role(role))) return false;

  send_message(socket, Message{"direct_ack", {{"room", room}}});
  auto peer_ack = recv_message_timeout(socket, kDirectPreflightTimeout, cancel);
  if (direct_cancelled(cancel)) return false;
  return peer_ack && direct_ack_ok(*peer_ack, room);
}

std::optional<TcpSocket> dial_direct_candidate(const DirectCandidate& candidate, std::chrono::milliseconds connect_timeout,
                                               Role role, const std::string& room, AdaptivePuncher& puncher,
                                               const ConnectOptions& connect_options, const std::string& phase,
                                               const std::atomic_bool* cancel) {
  const auto start = now_ms();
  auto socket = connect_tcp(candidate.endpoint, connect_timeout, connect_options, cancel);
  if (!socket.valid()) {
    if (direct_cancelled(cancel)) return std::nullopt;
    puncher.observe(PunchObservation{phase, candidate.endpoint.to_string(), candidate.kind, candidate.priority, false,
                                     now_ms() - start, "connect_failed"});
    return std::nullopt;
  }

  try {
    const bool ok = complete_direct_dial_preflight(socket, role, room, cancel);
    if (direct_cancelled(cancel)) {
      socket.close();
      return std::nullopt;
    }
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
                                                         std::uint16_t local_port, const std::string& phase,
                                                         const std::atomic_bool* cancel) {
  if (candidate.kind != "public" || local_port == 0 || connect_options.proxy) return std::nullopt;

  auto same_port_options = connect_options;
  same_port_options.local_bind = Endpoint{"", local_port};

  auto same_port_candidate = candidate;
  same_port_candidate.kind = "public-same-port";
  add_direct_candidate_reason(same_port_candidate, "same_port_probe");
  return dial_direct_candidate(same_port_candidate, connect_timeout, role, room, puncher, same_port_options, phase,
                               cancel);
}

std::optional<TcpSocket> accept_direct_candidate(TcpListener& listener, std::chrono::milliseconds timeout,
                                                 const std::string& room, Role expected_role,
                                                 AdaptivePuncher& puncher, const std::string& phase,
                                                 const std::atomic_bool* cancel) {
  if (direct_cancelled(cancel)) return std::nullopt;
  auto accepted = listener.accept(timeout);
  if (!accepted.valid()) return std::nullopt;
  if (direct_cancelled(cancel)) {
    accepted.close();
    return std::nullopt;
  }

  try {
    std::string accepted_endpoint = "listener";
    try {
      accepted_endpoint = accepted.peer_endpoint().to_string();
    } catch (const std::exception&) {
    }
    auto hello = recv_message_timeout(accepted, kDirectPreflightTimeout, cancel);
    if (direct_cancelled(cancel)) {
      accepted.close();
      return std::nullopt;
    }
    const bool ok = hello && direct_hello_ok(*hello, room, expected_role);
    if (ok) {
      send_message(accepted, Message{"direct_ack", {{"room", room}}});
      puncher.observe(PunchObservation{phase, accepted_endpoint, "accept", 0, true, 0, ""});
      return accepted;
    }
    puncher.observe(PunchObservation{phase, accepted_endpoint, "accept", 0, false, 0, "direct_hello_mismatch"});
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
                                                         connect_options, local_port, phase + "-same-port", cancel)) {
          return socket;
        }
        const auto remaining_timeout = candidate_connect_timeout(candidate, plan.connect_timeout, phase_deadline);
        if (remaining_timeout.count() <= 0) break;
        if (auto socket = dial_direct_candidate(candidate, remaining_timeout, self_role, room, puncher,
                                               connect_options, phase, cancel)) {
          return socket;
        }
      }
    }

    const auto remaining = remaining_until(phase_deadline);
    if (remaining.count() <= 0) break;
    const auto accept_timeout = std::min<std::chrono::milliseconds>(remaining, std::chrono::milliseconds(80));
    if (auto accepted =
            accept_direct_candidate(listener, accept_timeout, room, active_role, puncher, phase + "-accept", cancel)) {
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

  const auto try_burst = [&]() -> std::optional<TcpSocket> {
    for (const auto& candidate : candidates) {
      if (direct_cancelled(cancel)) break;
      const auto same_port_connect =
          plan.same_port_connect_timeout.count() > 0 ? plan.same_port_connect_timeout : kSyncSamePortConnectWindow;
      const auto timeout = std::min(candidate_connect_timeout(candidate, plan.connect_timeout, phase_deadline),
                                    same_port_connect);
      if (timeout.count() <= 0) break;
      if (auto socket = dial_direct_candidate_same_port(candidate, timeout, role, room, puncher, connect_options,
                                                       local_port, "sync-same-port", cancel)) {
        return socket;
      }
    }
    return std::nullopt;
  };

  if (const auto punch_at = parse_u64_strict(punch_token)) {
    constexpr int kBurstOffsetsMs[] = {-80, 0, 80, 160};
    for (const int offset : kBurstOffsetsMs) {
      if (direct_cancelled(cancel) || std::chrono::steady_clock::now() >= phase_deadline) break;
      if (!wait_until_punch_target(punch_target_with_offset(*punch_at, offset), phase_deadline, cancel)) break;
      if (auto socket = try_burst()) return socket;
      const auto remaining = remaining_until(phase_deadline);
      if (remaining.count() <= 0) break;
      const auto accept_timeout = std::min<std::chrono::milliseconds>(remaining, std::chrono::milliseconds(30));
      if (auto accepted =
              accept_direct_candidate(listener, accept_timeout, room, peer_role(role), puncher, "sync-same-port-accept",
                                      cancel)) {
        return accepted;
      }
    }
  }

  while (!direct_cancelled(cancel) && std::chrono::steady_clock::now() < phase_deadline) {
    if (auto socket = try_burst()) return socket;
    const auto remaining = remaining_until(phase_deadline);
    if (remaining.count() <= 0) break;
    const auto accept_timeout = std::min<std::chrono::milliseconds>(remaining, std::chrono::milliseconds(40));
    if (auto accepted =
            accept_direct_candidate(listener, accept_timeout, room, peer_role(role), puncher, "sync-same-port-accept",
                                    cancel)) {
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
  const auto same_port_budget = std::min<std::chrono::milliseconds>(plan.same_port_timeout, plan.total_timeout / 3);
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

void fill_success_address(PunchStats& stats, const std::string& candidate) {
  stats.successful_candidate_endpoint = candidate;
  try {
    const auto endpoint = parse_endpoint(candidate);
    stats.successful_candidate_family = ip_address_family_name(ip_address_family(endpoint.host));
    stats.successful_candidate_scope = ip_address_scope_name(ip_address_scope(endpoint.host));
  } catch (const KikoError&) {
    stats.successful_candidate_family = "unknown";
    stats.successful_candidate_scope = "unknown";
  }
}

std::string successful_direct_kind_from_observation(const PunchObservation& observation) {
  if (observation.kind != "accept") return observation.kind;
  try {
    const auto endpoint = parse_endpoint(observation.candidate);
    const auto family = ip_address_family(endpoint.host);
    const auto scope = ip_address_scope(endpoint.host);
    if (family == IpAddressFamily::IPv6 && scope == IpAddressScope::Global) return "ipv6_global";
    if (scope == IpAddressScope::Loopback || scope == IpAddressScope::Private ||
        scope == IpAddressScope::LinkLocal || scope == IpAddressScope::UniqueLocal) {
      return "lan";
    }
    if (scope == IpAddressScope::Global) return "public";
  } catch (const KikoError&) {
  }
  return observation.kind;
}

PunchStats punch_stats_from(const AdaptivePuncher& puncher, bool direct_ok, bool attempted) {
  PunchStats stats;
  stats.attempted = attempted;
  stats.direct_ok = direct_ok;
  if (!attempted) return stats;

  for (const auto& observation : puncher.observations()) {
    if (!observation.kind.empty()) ++stats.candidate_attempts_by_kind[observation.kind];
    if (observation.kind == "public-same-port") {
      ++stats.same_port_attempts;
      stats.same_port_last_elapsed_ms = static_cast<std::int64_t>(observation.elapsed_ms);
      if (observation.success) {
        ++stats.same_port_successes;
      } else {
        ++stats.same_port_failures;
      }
    }
    if (observation.success) {
      const auto successful_kind = successful_direct_kind_from_observation(observation);
      if (stats.successful_candidate_kind.empty() || stats.successful_candidate_kind == "accept") {
        stats.successful_candidate_kind = successful_kind;
        fill_success_address(stats, observation.candidate);
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

}  // namespace kiko
