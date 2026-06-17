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
  std::string last_direct_candidate_kind;
  std::int64_t last_direct_rtt_ms = -1;
  std::map<std::string, int> candidate_failures_by_kind;
};

[[nodiscard]] std::optional<NetworkProfileEntry> load_profile(const std::string& fingerprint);
void save_profile_success(const std::string& fingerprint, const std::string& path);
void save_profile_success(const std::string& fingerprint, const std::string& path, const PunchStats& stats);
void apply_profile_to_snapshot(const NetworkProfileEntry& profile, ConnectivitySnapshot& snapshot);
void apply_profile_candidate_bias(const NetworkProfileEntry& profile, std::vector<DirectCandidate>& candidates);
[[nodiscard]] std::string network_fingerprint();

}  // namespace kiko
