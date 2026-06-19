#include "adaptive.hpp"

#include <algorithm>
#include <sstream>

namespace kiko {
namespace {

std::chrono::milliseconds clamp_ms(std::chrono::milliseconds value, std::chrono::milliseconds min_value,
                                   std::chrono::milliseconds max_value) {
  return std::min(max_value, std::max(min_value, value));
}

std::string default_candidate_reason(const std::string& kind) {
  if (kind == "discovered") return "lan_discovery";
  if (kind == "lan") return "peer_lan_candidate";
  if (kind == "listen") return "peer_listen_host";
  if (kind == "public") return "relay_observed_public";
  if (kind == "public-same-port") return "same_port_probe";
  if (kind == "accept") return "inbound_accept";
  return "candidate";
}

bool has_reason(const DirectCandidate& candidate, const std::string& reason) {
  return std::find(candidate.reasons.begin(), candidate.reasons.end(), reason) != candidate.reasons.end();
}

std::chrono::milliseconds dial_timeout_for(const DirectCandidate& candidate,
                                           std::chrono::milliseconds plan_connect_timeout) {
  if (candidate.priority >= 90 || candidate.kind == "discovered" || candidate.kind == "lan" ||
      has_reason(candidate, "profile_direct_success")) {
    return clamp_ms(plan_connect_timeout + std::chrono::milliseconds(100), std::chrono::milliseconds(300),
                    std::chrono::milliseconds(600));
  }
  if (candidate.priority >= 60 || candidate.kind == "listen") {
    return clamp_ms(plan_connect_timeout, std::chrono::milliseconds(220), std::chrono::milliseconds(450));
  }
  if (candidate.kind == "public" || candidate.kind == "public-same-port") {
    return clamp_ms(plan_connect_timeout, std::chrono::milliseconds(120), std::chrono::milliseconds(220));
  }
  return clamp_ms(plan_connect_timeout, std::chrono::milliseconds(150), std::chrono::milliseconds(400));
}

void assign_candidate_dial_timeouts(PunchPlan& plan) {
  for (auto& candidate : plan.candidates) {
    candidate.reasons.erase(std::remove(candidate.reasons.begin(), candidate.reasons.end(), "short_probe"),
                            candidate.reasons.end());
    candidate.reasons.erase(std::remove(candidate.reasons.begin(), candidate.reasons.end(), "extended_probe"),
                            candidate.reasons.end());
    candidate.connect_timeout = dial_timeout_for(candidate, plan.connect_timeout);
    if (candidate.connect_timeout < plan.connect_timeout) {
      add_direct_candidate_reason(candidate, "short_probe");
    } else if (candidate.connect_timeout > plan.connect_timeout) {
      add_direct_candidate_reason(candidate, "extended_probe");
    }
  }
}

}  // namespace

NatProfile classify_nat(const std::vector<std::string>& local_addresses, const Endpoint& reflexive) {
  NatProfile profile;
  if (reflexive.host.empty() || reflexive.port == 0) {
    profile.type = NatType::Unknown;
    return profile;
  }
  bool public_is_local = std::find(local_addresses.begin(), local_addresses.end(), reflexive.host) != local_addresses.end();
  profile.type = public_is_local ? NatType::Open : NatType::BehindNat;
  return profile;
}

std::string nat_type_name(NatType type) {
  switch (type) {
    case NatType::Open:
      return "open";
    case NatType::BehindNat:
      return "behind-nat";
    default:
      return "unknown";
  }
}

DirectCandidate make_direct_candidate(Endpoint endpoint, std::string kind, int priority) {
  DirectCandidate candidate;
  candidate.endpoint = std::move(endpoint);
  candidate.kind = std::move(kind);
  candidate.priority = priority;
  candidate.reasons.push_back(default_candidate_reason(candidate.kind));
  return candidate;
}

void add_direct_candidate_reason(DirectCandidate& candidate, std::string reason) {
  if (reason.empty()) return;
  if (std::find(candidate.reasons.begin(), candidate.reasons.end(), reason) == candidate.reasons.end()) {
    candidate.reasons.push_back(std::move(reason));
  }
}

void tune_direct_candidate_timeouts(PunchPlan& plan) { assign_candidate_dial_timeouts(plan); }

PunchPlan AdaptivePuncher::plan(Role role, const std::vector<DirectCandidate>& candidates) const {
  PunchPlan plan;
  plan.candidates = candidates;
  std::stable_sort(plan.candidates.begin(), plan.candidates.end(),
                   [](const DirectCandidate& a, const DirectCandidate& b) {
                     return a.priority > b.priority;
                   });
  plan.prefer_outbound = role == Role::Receiver;

  // Receivers usually join second, so they can be more aggressive about dialing.
  if (role == Role::Receiver) {
    plan.connect_timeout = std::chrono::milliseconds(350);
    plan.retry_delay = std::chrono::milliseconds(80);
  }

  // If earlier candidates timed out, spend less time before falling back to relay.
  int failures = 0;
  for (const auto& observation : observations_) {
    if (!observation.success) ++failures;
  }
  if (failures >= 3) {
    plan.total_timeout = std::chrono::milliseconds(1800);
    plan.connect_timeout = std::chrono::milliseconds(250);
  }
  tune_direct_candidate_timeouts(plan);
  return plan;
}

PunchPlan AdaptivePuncher::plan(Role role, const std::vector<DirectCandidate>& candidates, const NatProfile& self,
                                const NatProfile& peer) const {
  PunchPlan plan = this->plan(role, candidates);

  // If either side is openly reachable, invest more in direct attempts.
  if (self.type == NatType::Open || peer.type == NatType::Open) {
    plan.total_timeout = std::chrono::milliseconds(3500);
    plan.connect_timeout = std::chrono::milliseconds(500);
  } else if (self.type == NatType::BehindNat && peer.type == NatType::BehindNat) {
    // Double-NAT TCP punching rarely succeeds; bias toward the relay sooner,
    // but still give LAN candidates a quick shot.
    plan.total_timeout = std::chrono::milliseconds(1200);
    plan.connect_timeout = std::chrono::milliseconds(250);
    plan.retry_delay = std::chrono::milliseconds(70);
  }
  tune_direct_candidate_timeouts(plan);
  return plan;
}

void AdaptivePuncher::observe(const PunchObservation& observation) { observations_.push_back(observation); }

std::string AdaptivePuncher::report() const {
  if (observations_.empty()) return "no punch observations\n";
  std::ostringstream oss;
  for (const auto& observation : observations_) {
    if (!observation.phase.empty()) oss << "phase=" << observation.phase << " ";
    oss << observation.candidate;
    if (!observation.kind.empty()) oss << " kind=" << observation.kind;
    if (observation.priority != 0) oss << " priority=" << observation.priority;
    oss << " success=" << (observation.success ? "true" : "false") << " elapsed_ms=" << observation.elapsed_ms;
    if (!observation.error.empty()) oss << " error=" << observation.error;
    oss << "\n";
  }
  return oss.str();
}

std::string role_name(Role role) { return role == Role::Sender ? "sender" : "receiver"; }

Role parse_role(const std::string& value) {
  if (value == "sender") return Role::Sender;
  if (value == "receiver") return Role::Receiver;
  throw KikoError("unknown role: " + value);
}

}  // namespace kiko
