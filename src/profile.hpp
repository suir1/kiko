#pragma once

#include "connectivity.hpp"

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
  std::string last_relay_path;
  std::string last_relay_interface;
  std::string last_relay_reason;
  std::map<std::string, std::int64_t> relay_rtt_by_path;
  std::string last_direct_candidate_kind;
  std::int64_t last_direct_rtt_ms = -1;
  std::map<std::string, int> candidate_failures_by_kind;
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
void apply_profile_to_snapshot(const NetworkProfileEntry& profile, ConnectivitySnapshot& snapshot);
void apply_profile_candidate_bias(const NetworkProfileEntry& profile, std::vector<DirectCandidate>& candidates);
[[nodiscard]] std::string network_fingerprint();

}  // namespace kiko
