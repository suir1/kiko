#include "adaptive.hpp"

#include <algorithm>
#include <sstream>

namespace kiko {

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
  return DirectCandidate{std::move(endpoint), std::move(kind), priority};
}

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
