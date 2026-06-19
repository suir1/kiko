#include "direct_session.hpp"

#include "profile.hpp"
#include "progress.hpp"
#include "udp_punch.hpp"

#include <algorithm>
#include <sstream>

namespace kiko {
namespace {

constexpr auto kPublicOnlyDirectWindow = std::chrono::milliseconds(500);
constexpr auto kPublicOnlyConnectWindow = std::chrono::milliseconds(220);

bool is_high_confidence_direct_candidate(const DirectCandidate& candidate) {
  return candidate.kind == "discovered" || candidate.kind == "lan" || candidate.kind == "listen" ||
         candidate.kind == "manual";
}

void apply_relay_fallback_guard(PunchPlan& punch) {
  if (punch.candidates.empty()) return;
  if (std::any_of(punch.candidates.begin(), punch.candidates.end(), is_high_confidence_direct_candidate)) return;

  bool changed = false;
  if (punch.total_timeout > kPublicOnlyDirectWindow) {
    punch.total_timeout = kPublicOnlyDirectWindow;
    changed = true;
  }
  if (punch.connect_timeout > kPublicOnlyConnectWindow) {
    punch.connect_timeout = kPublicOnlyConnectWindow;
    changed = true;
  }
  if (!changed) return;

  for (auto& candidate : punch.candidates) add_direct_candidate_reason(candidate, "relay_fallback_guard");
  tune_direct_candidate_timeouts(punch);
}

std::vector<DirectCandidate> peer_candidates(const Message& peer, const std::vector<Endpoint>& extra = {}) {
  std::vector<DirectCandidate> out;
  auto unspecified_host = [](const std::string& host) {
    return host == "0.0.0.0" || host == "::" || host == "[::]";
  };
  auto push_unique = [&](Endpoint e, std::string kind, int priority) {
    if (e.port == 0 || e.host.empty() || unspecified_host(e.host)) return;
    for (const auto& existing : out) {
      if (existing.endpoint.host == e.host && existing.endpoint.port == e.port) return;
    }
    out.push_back(make_direct_candidate(std::move(e), std::move(kind), priority));
  };

  for (const auto& e : extra) push_unique(e, "discovered", 95);

  auto listen_port = message_port_field(peer, "peer_listen_port");
  if (listen_port) {
    for (const auto& host : split_csv(peer.get("peer_local_candidates"))) {
      push_unique(Endpoint{host, *listen_port}, "lan", 90);
    }
    if (!peer.get("peer_listen_host").empty()) {
      push_unique(Endpoint{peer.get("peer_listen_host"), *listen_port}, "listen", 60);
    }
  }

  auto public_port = message_port_field(peer, "peer_public_port");
  if (!peer.get("peer_public_host").empty() && public_port) {
    push_unique(Endpoint{peer.get("peer_public_host"), *public_port}, "public", 20);
  }
  return out;
}

std::string describe_direct_plan(const RoutePlan& route_plan, const PunchPlan& punch) {
  std::ostringstream oss;
  oss << "direct plan: timeout=" << punch.total_timeout.count() << "ms connect=" << punch.connect_timeout.count()
      << "ms";
  if (route_plan.udp_punch_enabled) oss << " udp-assist";
  if (punch.candidates.empty()) {
    oss << " candidates=none";
    return oss.str();
  }

  oss << " candidates=";
  for (std::size_t i = 0; i < punch.candidates.size(); ++i) {
    const auto& candidate = punch.candidates[i];
    if (i > 0) oss << ",";
    oss << candidate.kind << "@" << candidate.endpoint.to_string() << " score=" << candidate.priority;
    if (candidate.connect_timeout.count() > 0 && candidate.connect_timeout != punch.connect_timeout) {
      oss << " dial=" << candidate.connect_timeout.count() << "ms";
    }
    if (!candidate.reasons.empty()) {
      oss << " reason=";
      for (std::size_t j = 0; j < candidate.reasons.size(); ++j) {
        if (j > 0) oss << "+";
        oss << candidate.reasons[j];
      }
    }
  }
  return oss.str();
}

std::vector<TcpSocket> gather_direct_mux_aux_channels(Role role, TcpListener& listener, const Message& peer,
                                                      int connections, const std::string& room,
                                                      const ConnectOptions& connect_options,
                                                      std::chrono::milliseconds setup_timeout) {
  if (connections <= 1) return {};
  const auto deadline = std::chrono::steady_clock::now() + setup_timeout;
  auto remaining = [&]() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
  };

  std::vector<TcpSocket> aux(static_cast<std::size_t>(connections - 1));
  if (role == Role::Sender) {
    std::vector<bool> seen(static_cast<std::size_t>(connections), false);
    seen[0] = true;
    for (int accepted_count = 1; accepted_count < connections; ++accepted_count) {
      auto wait = remaining();
      if (wait.count() <= 0) throw KikoError("timed out accepting auxiliary direct connection");
      auto accepted = listener.accept(std::min(wait, std::chrono::milliseconds(500)));
      if (!accepted.valid()) throw KikoError("failed to accept auxiliary direct connection");

      wait = remaining();
      if (wait.count() <= 0) throw KikoError("timed out reading auxiliary direct hello");
      auto hello = recv_message_timeout(accepted, std::min(wait, std::chrono::milliseconds(500)));
      if (!hello || hello->type != "direct_aux" || hello->get("room") != room) {
        throw KikoError("invalid auxiliary direct connection");
      }

      const auto index = hello->get_u64("conn_index", 0);
      if (index == 0 || index >= static_cast<std::uint64_t>(connections) || seen[static_cast<std::size_t>(index)]) {
        throw KikoError("invalid auxiliary direct connection index");
      }
      seen[static_cast<std::size_t>(index)] = true;
      aux[static_cast<std::size_t>(index - 1)] = std::move(accepted);
    }
    return aux;
  }

  const auto candidates = peer_candidates(peer);
  if (candidates.empty()) throw KikoError("peer listen candidates missing for direct mux");
  for (int k = 1; k < connections; ++k) {
    TcpSocket connected;
    for (const auto& candidate : candidates) {
      auto wait = remaining();
      if (wait.count() <= 0) break;
      connected = connect_tcp(candidate.endpoint, std::min(wait, std::chrono::milliseconds(700)), connect_options);
      if (connected.valid()) break;
    }
    if (!connected.valid()) throw KikoError("failed to open auxiliary direct connection");
    send_message(connected, Message{"direct_aux", {{"room", room}, {"conn_index", std::to_string(k)}}});
    aux[static_cast<std::size_t>(k - 1)] = std::move(connected);
  }
  return aux;
}

bool direct_mux_status_ok(const Message& status, const std::string& room, int connections) {
  return status.type == "direct_mux_status" && status.get("room") == room &&
         status.get_u64("conn_count", 0) == static_cast<std::uint64_t>(connections);
}

}  // namespace

std::optional<TcpSocket> attempt_direct(Role role, TcpListener& listener, const Message& peer,
                                        const std::vector<Endpoint>& lan_extra, AdaptivePuncher& puncher,
                                        const NatProfile& self, const NatProfile& peer_nat,
                                        const RoutePlan& route_plan, const std::string& room,
                                        const ConnectOptions& connect_options, ProgressReporter* reporter,
                                        const std::atomic_bool* cancel) {
  if (route_plan.skip_direct) return std::nullopt;
  PunchPlan punch;
  auto candidates = peer_candidates(peer, lan_extra);
  if (auto profile = load_profile(network_fingerprint())) apply_profile_candidate_bias(*profile, candidates);
  apply_route_plan_to_adaptive(route_plan, role, puncher, candidates, self, peer_nat, punch);
  apply_relay_fallback_guard(punch);
  if (reporter) reporter->status(describe_direct_plan(route_plan, punch));
  if (route_plan.udp_punch_enabled) {
    Endpoint peer_wan{peer.get("peer_public_host"), message_port_or(peer, "peer_public_port", 0, true)};
    return try_udp_assisted_direct(role, listener, peer_wan, peer.get("punch_token"), punch, puncher, room,
                                   connect_options, cancel);
  }
  return try_direct_with_plan(role, listener, punch, puncher, room, connect_options, peer.get("punch_token"), cancel);
}

DirectMuxResult negotiate_direct_mux_channels(TcpSocket primary, Role role, TcpListener& listener, const Message& peer,
                                              int connections, const std::string& room,
                                              const ConnectOptions& connect_options,
                                              std::chrono::milliseconds setup_timeout) {
  DirectMuxResult result;
  if (connections <= 1) {
    result.channels.push_back(std::move(primary));
    return result;
  }
  if (setup_timeout.count() <= 0) setup_timeout = std::chrono::milliseconds(1);

  std::vector<TcpSocket> aux;
  bool local_ready = false;
  std::string local_error;
  try {
    aux = gather_direct_mux_aux_channels(role, listener, peer, connections, room, connect_options, setup_timeout);
    local_ready = aux.size() == static_cast<std::size_t>(connections - 1);
    if (!local_ready) local_error = "auxiliary direct connection count mismatch";
  } catch (const std::exception& e) {
    local_error = e.what();
  }

  Message status{"direct_mux_status",
                 {{"room", room}, {"conn_count", std::to_string(connections)}, {"ready", local_ready ? "1" : "0"}}};
  if (!local_error.empty()) status.fields["error"] = local_error;

  bool peer_ready = false;
  std::string peer_error;
  try {
    send_message(primary, status);
    const auto status_timeout = setup_timeout + std::chrono::seconds(2);
    auto peer_status = recv_message_timeout(primary, status_timeout);
    if (peer_status && direct_mux_status_ok(*peer_status, room, connections)) {
      peer_ready = peer_status->get("ready") == "1";
      peer_error = peer_status->get("error");
    } else {
      peer_error = "peer mux status missing";
    }
  } catch (const std::exception& e) {
    peer_error = e.what();
  }

  if (local_ready && peer_ready) {
    result.mux_enabled = true;
    result.channels.reserve(static_cast<std::size_t>(connections));
    result.channels.push_back(std::move(primary));
    for (auto& channel : aux) result.channels.push_back(std::move(channel));
    return result;
  }

  for (auto& channel : aux) channel.close();
  result.channels.push_back(std::move(primary));
  result.fallback_reason = !local_error.empty() ? local_error : peer_error;
  if (result.fallback_reason.empty()) result.fallback_reason = "peer declined direct mux";
  return result;
}

}  // namespace kiko
