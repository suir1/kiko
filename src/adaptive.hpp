#pragma once

#include "common.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace kiko {

enum class Role { Sender, Receiver };

// Coarse reachability class derived by comparing a host's own local interface
// addresses with the reflexive (relay-observed) address. Full STUN-style
// symmetric/cone detection is out of scope.
enum class NatType { Open, BehindNat, Unknown };

struct NatProfile {
  NatType type = NatType::Unknown;
};

// Open when the reflexive IP is one of the host's local addresses (no NAT in the
// path); BehindNat when it differs; Unknown without a reflexive observation.
[[nodiscard]] NatProfile classify_nat(const std::vector<std::string>& local_addresses, const Endpoint& reflexive);
[[nodiscard]] std::string nat_type_name(NatType type);

struct PunchObservation {
  std::string phase;
  std::string candidate;
  std::string kind;
  int priority = 0;
  bool success = false;
  std::uint64_t elapsed_ms = 0;
  std::string error;
};

struct DirectCandidate {
  Endpoint endpoint;
  std::string kind = "unknown";
  int priority = 0;
  std::chrono::milliseconds connect_timeout{0};
  std::vector<std::string> reasons;
};

[[nodiscard]] DirectCandidate make_direct_candidate(Endpoint endpoint, std::string kind, int priority);
void add_direct_candidate_reason(DirectCandidate& candidate, std::string reason);

struct PunchPlan {
  std::chrono::milliseconds total_timeout{2500};
  std::chrono::milliseconds connect_timeout{450};
  std::chrono::milliseconds same_port_timeout{500};
  std::chrono::milliseconds same_port_connect_timeout{160};
  std::chrono::milliseconds retry_delay{120};
  bool prefer_outbound = true;
  std::vector<DirectCandidate> candidates;
};

void tune_direct_candidate_timeouts(PunchPlan& plan);

class AdaptivePuncher {
 public:
  PunchPlan plan(Role role, const std::vector<DirectCandidate>& candidates) const;
  // NAT-aware planning: `self`/`peer` profiles tune how long to attempt direct
  // connections before falling back to the relay.
  PunchPlan plan(Role role, const std::vector<DirectCandidate>& candidates, const NatProfile& self,
                 const NatProfile& peer) const;
  void observe(const PunchObservation& observation);
  [[nodiscard]] std::string report() const;
  [[nodiscard]] const std::vector<PunchObservation>& observations() const { return observations_; }

 private:
  std::vector<PunchObservation> observations_;
};

[[nodiscard]] std::string role_name(Role role);
[[nodiscard]] Role parse_role(const std::string& value);

}  // namespace kiko
