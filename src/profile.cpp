#include "profile.hpp"

#include "socket.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <utility>

namespace kiko {
namespace {

std::filesystem::path profile_path() {
  if (const char* path = std::getenv("KIKO_PROFILE_PATH")) {
    if (path[0] != '\0') return std::filesystem::path(path);
  }
  if (const char* home = std::getenv("HOME")) {
    return std::filesystem::path(home) / ".config" / "kiko" / "profile.json";
  }
  return std::filesystem::path(".kiko_profile.json");
}

std::map<std::string, int> parse_failures_by_kind(const nlohmann::json& object) {
  std::map<std::string, int> out;
  if (!object.is_object()) return out;
  for (auto it = object.begin(); it != object.end(); ++it) {
    if (it.key().empty() || !it.value().is_number_integer()) continue;
    const auto count = it.value().get<int>();
    if (count > 0) out[it.key()] = count;
  }
  return out;
}

std::map<std::string, std::int64_t> parse_rtt_by_path(const nlohmann::json& object) {
  std::map<std::string, std::int64_t> out;
  if (!object.is_object()) return out;
  for (auto it = object.begin(); it != object.end(); ++it) {
    if (it.key().empty() || !it.value().is_number_integer()) continue;
    const auto rtt = it.value().get<std::int64_t>();
    if (rtt >= 0) out[it.key()] = rtt;
  }
  return out;
}

nlohmann::json failures_by_kind_json(const std::map<std::string, int>& failures) {
  nlohmann::json out = nlohmann::json::object();
  for (const auto& [kind, count] : failures) {
    if (!kind.empty() && count > 0) out[kind] = count;
  }
  return out;
}

nlohmann::json rtt_by_path_json(const std::map<std::string, std::int64_t>& rtts) {
  nlohmann::json out = nlohmann::json::object();
  for (const auto& [path, rtt] : rtts) {
    if (!path.empty() && rtt >= 0) out[path] = rtt;
  }
  return out;
}

void save_profile_success_impl(const std::string& fingerprint, const std::string& path, const PunchStats* stats,
                               const ProfileRelayPath* relay) {
  nlohmann::json root;
  std::ifstream in(profile_path());
  if (in) {
    try {
      in >> root;
    } catch (...) {
      root = nlohmann::json::object();
    }
  }
  const int prev = root.contains(fingerprint) ? root[fingerprint].value("success_count", 0) : 0;
  auto entry = root.contains(fingerprint) && root[fingerprint].is_object() ? root[fingerprint] : nlohmann::json::object();
  entry["last_path"] = path;
  entry["success_count"] = prev + 1;

  if (relay) {
    if (!relay->path.empty()) entry["last_relay_path"] = relay->path;
    if (!relay->bind_interface.empty()) {
      entry["last_relay_interface"] = relay->bind_interface;
    } else {
      entry.erase("last_relay_interface");
    }
    if (!relay->reason.empty()) entry["last_relay_reason"] = relay->reason;
    if (!relay->rtt_by_path.empty()) entry["relay_rtt_by_path"] = rtt_by_path_json(relay->rtt_by_path);
  }

  if (stats) {
    if (path == "direct" && !stats->successful_candidate_kind.empty() && stats->successful_candidate_kind != "accept") {
      entry["last_direct_candidate_kind"] = stats->successful_candidate_kind;
      if (stats->successful_elapsed_ms >= 0) entry["last_direct_rtt_ms"] = stats->successful_elapsed_ms;
    }
    if (!stats->candidate_failures_by_kind.empty()) {
      entry["candidate_failures_by_kind"] = failures_by_kind_json(stats->candidate_failures_by_kind);
    }
  }

  root[fingerprint] = std::move(entry);
  std::error_code ec;
  std::filesystem::create_directories(profile_path().parent_path(), ec);
  std::ofstream out(profile_path());
  if (out) out << root.dump(2);
}

}  // namespace

std::string network_fingerprint() {
  const auto addrs = local_lan_candidate_addresses();
  if (addrs.empty()) return "unknown";
  return addrs.front();
}

std::optional<NetworkProfileEntry> load_profile(const std::string& fingerprint) {
  std::ifstream in(profile_path());
  if (!in) return std::nullopt;
  nlohmann::json root;
  try {
    in >> root;
  } catch (...) {
    return std::nullopt;
  }
  if (!root.contains(fingerprint)) return std::nullopt;
  NetworkProfileEntry entry;
  entry.fingerprint = fingerprint;
  entry.last_path = root[fingerprint].value("last_path", "");
  entry.success_count = root[fingerprint].value("success_count", 0);
  entry.last_relay_path = root[fingerprint].value("last_relay_path", "");
  entry.last_relay_interface = root[fingerprint].value("last_relay_interface", "");
  entry.last_relay_reason = root[fingerprint].value("last_relay_reason", "");
  if (root[fingerprint].contains("relay_rtt_by_path")) {
    entry.relay_rtt_by_path = parse_rtt_by_path(root[fingerprint]["relay_rtt_by_path"]);
  }
  entry.last_direct_candidate_kind = root[fingerprint].value("last_direct_candidate_kind", "");
  entry.last_direct_rtt_ms = root[fingerprint].value("last_direct_rtt_ms", -1);
  if (root[fingerprint].contains("candidate_failures_by_kind")) {
    entry.candidate_failures_by_kind = parse_failures_by_kind(root[fingerprint]["candidate_failures_by_kind"]);
  }
  return entry;
}

void save_profile_success(const std::string& fingerprint, const std::string& path) {
  save_profile_success_impl(fingerprint, path, nullptr, nullptr);
}

void save_profile_success(const std::string& fingerprint, const std::string& path, const PunchStats& stats) {
  save_profile_success_impl(fingerprint, path, &stats, nullptr);
}

void save_profile_success(const std::string& fingerprint, const std::string& path, const ProfileRelayPath& relay) {
  save_profile_success_impl(fingerprint, path, nullptr, &relay);
}

void save_profile_success(const std::string& fingerprint, const std::string& path, const PunchStats& stats,
                          const ProfileRelayPath& relay) {
  save_profile_success_impl(fingerprint, path, &stats, &relay);
}

std::optional<OutboundHistory> outbound_history_from_profile(const NetworkProfileEntry& profile) {
  if (profile.last_relay_path.empty() && profile.last_relay_interface.empty() && profile.last_relay_reason.empty() &&
      profile.relay_rtt_by_path.empty()) {
    return std::nullopt;
  }
  OutboundHistory history;
  history.path = profile.last_relay_path;
  history.bind_interface = profile.last_relay_interface;
  history.reason = profile.last_relay_reason;
  history.rtt_by_path = profile.relay_rtt_by_path;
  return history;
}

void apply_profile_to_snapshot(const NetworkProfileEntry& profile, ConnectivitySnapshot& snapshot) {
  snapshot.profile_last_path = profile.last_path;
  snapshot.profile_success_count = profile.success_count;
  snapshot.profile_relay_path = profile.last_relay_path;
  snapshot.profile_relay_interface = profile.last_relay_interface;
  snapshot.profile_relay_reason = profile.last_relay_reason;
  snapshot.profile_relay_rtt_by_path = profile.relay_rtt_by_path;
  snapshot.profile_direct_candidate_kind = profile.last_direct_candidate_kind;
  snapshot.profile_direct_rtt_ms = profile.last_direct_rtt_ms;
  snapshot.profile_candidate_failures_by_kind = profile.candidate_failures_by_kind;
}

void apply_profile_candidate_bias(const NetworkProfileEntry& profile, std::vector<DirectCandidate>& candidates) {
  RoutePlan::DirectCandidateScoreHints hints;
  hints.profile_last_path = profile.last_path;
  hints.profile_direct_candidate_kind = profile.last_direct_candidate_kind;
  hints.profile_candidate_failures_by_kind = profile.candidate_failures_by_kind;
  apply_direct_candidate_scoring(candidates, hints);
}

}  // namespace kiko
