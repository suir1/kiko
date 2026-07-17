#include "connect/profile.hpp"

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

nlohmann::json load_profile_root(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in) return {};
  try {
    return nlohmann::json::parse(in);
  } catch (...) {
    return {};
  }
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

}  // namespace

void save_profile_success(const std::string& fingerprint, const ProfileSuccess& success) {
  const auto storage_path = profile_path();
  auto root = load_profile_root(storage_path);
  const int prev = root.contains(fingerprint) ? root[fingerprint].value("success_count", 0) : 0;
  auto entry = root.contains(fingerprint) && root[fingerprint].is_object() ? root[fingerprint] : nlohmann::json::object();
  const auto prev_path = entry.value("last_path", std::string{});
  const int prev_streak = entry.value("path_streak", prev_path == success.path ? prev : 0);
  entry["last_path"] = success.path;
  entry["success_count"] = prev + 1;
  entry["path_streak"] = prev_path == success.path ? std::max(0, prev_streak) + 1 : 1;

  if (success.outbound) {
    if (!success.outbound->path.empty()) entry["last_relay_path"] = success.outbound->path;
    if (!success.outbound->bind_interface.empty()) {
      entry["last_relay_interface"] = success.outbound->bind_interface;
    } else {
      entry.erase("last_relay_interface");
    }
    if (!success.outbound->reason.empty()) entry["last_relay_reason"] = success.outbound->reason;
    if (!success.outbound->rtt_by_path.empty()) {
      entry["relay_rtt_by_path"] = rtt_by_path_json(success.outbound->rtt_by_path);
    }
  }

  if (success.punch) {
    const auto& stats = *success.punch;
    if (success.path == "direct" && !stats.successful_candidate_kind.empty() &&
        stats.successful_candidate_kind != "accept") {
      entry["last_direct_candidate_kind"] = stats.successful_candidate_kind;
      if (stats.successful_elapsed_ms >= 0) entry["last_direct_rtt_ms"] = stats.successful_elapsed_ms;
    }
    if (!stats.candidate_failures_by_kind.empty()) {
      entry["candidate_failures_by_kind"] = failures_by_kind_json(stats.candidate_failures_by_kind);
    }
    if (stats.same_port_attempts > 0) {
      entry["same_port_attempts"] = entry.value("same_port_attempts", 0) + stats.same_port_attempts;
      entry["same_port_successes"] = entry.value("same_port_successes", 0) + stats.same_port_successes;
      if (stats.same_port_successes > 0) {
        entry["same_port_failure_streak"] = 0;
      } else {
        entry["same_port_failure_streak"] =
            entry.value("same_port_failure_streak", 0) + stats.same_port_failures;
      }
      if (stats.same_port_last_elapsed_ms >= 0) {
        entry["same_port_last_elapsed_ms"] = stats.same_port_last_elapsed_ms;
      }
    }
  }

  root[fingerprint] = std::move(entry);
  std::error_code ec;
  std::filesystem::create_directories(storage_path.parent_path(), ec);
  std::ofstream out(storage_path);
  if (out) out << root.dump(2);
}

std::string network_fingerprint(const NetworkInterfaceInventory& interfaces) {
  const auto addrs = interfaces.lan_candidate_addresses();
  if (addrs.empty()) return "unknown";
  return addrs.front();
}

std::optional<NetworkProfileEntry> load_profile(const std::string& fingerprint) {
  const auto root = load_profile_root(profile_path());
  if (!root.contains(fingerprint)) return std::nullopt;
  NetworkProfileEntry entry;
  entry.last_path = root[fingerprint].value("last_path", "");
  entry.success_count = root[fingerprint].value("success_count", 0);
  entry.path_streak = root[fingerprint].value("path_streak", entry.last_path.empty() ? 0 : entry.success_count);
  entry.outbound_history.path = root[fingerprint].value("last_relay_path", "");
  entry.outbound_history.bind_interface = root[fingerprint].value("last_relay_interface", "");
  entry.outbound_history.reason = root[fingerprint].value("last_relay_reason", "");
  if (root[fingerprint].contains("relay_rtt_by_path")) {
    entry.outbound_history.rtt_by_path = parse_rtt_by_path(root[fingerprint]["relay_rtt_by_path"]);
  }
  entry.last_direct_candidate_kind = root[fingerprint].value("last_direct_candidate_kind", "");
  entry.last_direct_rtt_ms = root[fingerprint].value("last_direct_rtt_ms", -1);
  if (root[fingerprint].contains("candidate_failures_by_kind")) {
    entry.candidate_failures_by_kind = parse_failures_by_kind(root[fingerprint]["candidate_failures_by_kind"]);
  }
  entry.same_port_attempts = root[fingerprint].value("same_port_attempts", 0);
  entry.same_port_successes = root[fingerprint].value("same_port_successes", 0);
  entry.same_port_failure_streak = root[fingerprint].value("same_port_failure_streak", 0);
  entry.same_port_last_elapsed_ms = root[fingerprint].value("same_port_last_elapsed_ms", -1);
  return entry;
}

std::optional<OutboundHistory> outbound_history_from_profile(const NetworkProfileEntry& profile) {
  const auto& history = profile.outbound_history;
  if (history.path.empty() && history.bind_interface.empty() && history.reason.empty() && history.rtt_by_path.empty()) {
    return std::nullopt;
  }
  return history;
}

}  // namespace kiko
