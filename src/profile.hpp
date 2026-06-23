#pragma once

#include "connectivity.hpp"
#include "outbound_policy.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace kiko {

struct NetworkProfileEntry {
  std::string fingerprint;
  std::string last_path;  // direct | relay | lan_relay
  int success_count = 0;
  int path_streak = 0;
  std::string last_relay_path;
  std::string last_relay_interface;
  std::string last_relay_reason;
  std::map<std::string, std::int64_t> relay_rtt_by_path;
  std::string last_direct_candidate_kind;
  std::int64_t last_direct_rtt_ms = -1;
  std::map<std::string, int> candidate_failures_by_kind;
  int same_port_attempts = 0;
  int same_port_successes = 0;
  int same_port_failure_streak = 0;
  std::int64_t same_port_last_elapsed_ms = -1;
};

struct ProfileRelayPath {
  std::string path;
  std::string bind_interface;
  std::string reason;
  std::map<std::string, std::int64_t> rtt_by_path;
};

[[nodiscard]] std::optional<NetworkProfileEntry> load_profile(const std::string& fingerprint);
void save_profile_success(const std::string& fingerprint, const std::string& path);
void save_profile_success(const std::string& fingerprint, const std::string& path, const PunchStats& stats);
void save_profile_success(const std::string& fingerprint, const std::string& path, const ProfileRelayPath& relay);
void save_profile_success(const std::string& fingerprint, const std::string& path, const PunchStats& stats,
                          const ProfileRelayPath& relay);
[[nodiscard]] std::optional<OutboundHistory> outbound_history_from_profile(const NetworkProfileEntry& profile);
void apply_profile_to_snapshot(const NetworkProfileEntry& profile, ConnectivitySnapshot& snapshot);
void apply_profile_candidate_bias(const NetworkProfileEntry& profile, std::vector<DirectCandidate>& candidates);
[[nodiscard]] std::string network_fingerprint();

}  // namespace kiko
