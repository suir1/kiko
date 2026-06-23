#pragma once

#include "adaptive.hpp"
#include "common.hpp"
#include "network_probe.hpp"
#include "protocol.hpp"
#include "proxy.hpp"
#include "relay_race.hpp"
#include "socket.hpp"
#include <atomic>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace kiko {

struct PunchStats {
  bool attempted = false;
  bool direct_ok = false;
  std::map<std::string, int> failures;
  std::string successful_candidate_kind;
  std::string successful_candidate_endpoint;
  std::string successful_candidate_family;
  std::string successful_candidate_scope;
  int successful_candidate_priority = 0;
  std::int64_t successful_elapsed_ms = -1;
  std::map<std::string, int> candidate_attempts_by_kind;
  std::map<std::string, int> candidate_failures_by_kind;
  int same_port_attempts = 0;
  int same_port_successes = 0;
  int same_port_failures = 0;
  std::int64_t same_port_last_elapsed_ms = -1;
};

struct ConnectivitySnapshot {
  NatType self_nat = NatType::Unknown;
  NatType peer_nat = NatType::Unknown;
  StunNatClass stun_nat = StunNatClass::Unknown;
  bool vpn_detected = false;
  std::size_t lan_discovered_count = 0;
  std::string gateway_hint;
  std::vector<RelayProbeEntry> relays;
  PunchStats punch;
  std::vector<std::string> lan_candidates;
  bool no_direct_config = false;
  bool only_local = false;
  std::string profile_last_path;
  int profile_success_count = 0;
  int profile_path_streak = 0;
  std::string profile_relay_path;
  std::string profile_relay_interface;
  std::string profile_relay_reason;
  std::map<std::string, std::int64_t> profile_relay_rtt_by_path;
  std::string profile_direct_candidate_kind;
  std::int64_t profile_direct_rtt_ms = -1;
  std::map<std::string, int> profile_candidate_failures_by_kind;
  int profile_same_port_attempts = 0;
  int profile_same_port_successes = 0;
  int profile_same_port_failure_streak = 0;
  std::int64_t profile_same_port_last_elapsed_ms = -1;
  std::size_t self_global_ipv6_count = 0;
  std::size_t peer_global_ipv6_count = 0;
  std::uint64_t total_bytes = 0;
  std::size_t file_count = 0;
  std::uint64_t largest_file_bytes = 0;
  double compressible_ratio = 0;  // 0–1 share of non-dir files that will use zstd
  int connections_hint = 0;       // rule/auto suggestion before AI override
};

struct RoutePlan {
  bool skip_direct = false;
  bool udp_punch_enabled = false;
  std::chrono::milliseconds direct_timeout{2500};
  std::chrono::milliseconds direct_connect{450};
  std::chrono::milliseconds same_port_timeout{500};
  std::chrono::milliseconds same_port_connect{160};
  int connections = 4;
  std::vector<std::string> direct_candidate_order;  // optional AI hint: manual/discovered/lan/listen/ipv6_global/public
  std::vector<std::string> relay_order;  // optional AI hint: embedded, lan, external
  struct DirectCandidateScoreHints {
    bool vpn_detected = false;
    std::string profile_last_path;
    std::string profile_direct_candidate_kind;
    std::map<std::string, int> profile_candidate_failures_by_kind;
  } candidate_score_hints;
  std::string reason;
};

class RuleScheduler {
 public:
  [[nodiscard]] RoutePlan plan(const ConnectivitySnapshot& snapshot, const std::optional<StunProbeResult>& stun,
                               bool force_no_direct, int default_connections) const;
};

[[nodiscard]] ConnectivitySnapshot build_pre_rendezvous_snapshot(bool no_direct, bool only_local,
                                                                 std::size_t lan_discovered_count,
                                                                 std::uint64_t total_bytes);

void apply_route_plan_to_adaptive(const RoutePlan& plan, Role role, AdaptivePuncher& puncher,
                                  const std::vector<DirectCandidate>& candidates, const NatProfile& self,
                                  const NatProfile& peer, PunchPlan& out);

void apply_direct_candidate_scoring(std::vector<DirectCandidate>& candidates,
                                    const RoutePlan::DirectCandidateScoreHints& hints,
                                    const std::vector<std::string>& kind_order = {});
void apply_direct_candidate_kind_order(std::vector<DirectCandidate>& candidates, const std::vector<std::string>& kind_order);

[[nodiscard]] std::optional<TcpSocket> try_direct_with_plan(Role role, TcpListener& listener, PunchPlan plan,
                                                            AdaptivePuncher& puncher, const std::string& room,
                                                            const ConnectOptions& connect_options = ConnectOptions{},
                                                            const std::string& punch_token = "",
                                                            const std::atomic_bool* cancel = nullptr);

[[nodiscard]] PunchStats punch_stats_from(const AdaptivePuncher& puncher, bool direct_ok, bool attempted);

}  // namespace kiko
