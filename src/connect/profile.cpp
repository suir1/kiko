#include "connect/profile.hpp"

#include "platform/atomic_file.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
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

enum class ProfileReadStatus { Missing, Loaded, Invalid, Unreadable };

struct ProfileReadResult {
  ProfileReadStatus status = ProfileReadStatus::Missing;
  nlohmann::json root = nlohmann::json::object();
  std::string error;
};

ProfileReadResult load_profile_root(const std::filesystem::path& path) {
  std::error_code status_error;
  const auto status = std::filesystem::symlink_status(path, status_error);
  if (status_error == std::errc::no_such_file_or_directory) return {};
  if (!status_error && !std::filesystem::exists(status)) return {};
  if (status_error) return {ProfileReadStatus::Unreadable, nlohmann::json::object(), status_error.message()};
  if (!std::filesystem::is_regular_file(status)) {
    return {ProfileReadStatus::Unreadable, nlohmann::json::object(), "path is not a regular file"};
  }

  std::ifstream in(path);
  if (!in) return {ProfileReadStatus::Unreadable, nlohmann::json::object(), "could not open file"};
  try {
    auto root = nlohmann::json::parse(in);
    if (!root.is_object()) return {ProfileReadStatus::Invalid, {}, "root must be a JSON object"};
    return {ProfileReadStatus::Loaded, std::move(root), {}};
  } catch (const std::exception& error) {
    return {ProfileReadStatus::Invalid, {}, error.what()};
  }
}

std::optional<std::uint64_t> parse_nonnegative_integer(const nlohmann::json& value) {
  if (value.is_number_unsigned()) return value.get<std::uint64_t>();
  if (!value.is_number_integer()) return std::nullopt;
  const auto signed_value = value.get<std::int64_t>();
  if (signed_value < 0) return std::nullopt;
  return static_cast<std::uint64_t>(signed_value);
}

int profile_int_field(const nlohmann::json& object, const char* key, int fallback = 0) {
  if (!object.is_object() || !object.contains(key)) return fallback;
  const auto value = parse_nonnegative_integer(object.at(key));
  if (!value) return fallback;
  return *value > static_cast<std::uint64_t>(std::numeric_limits<int>::max())
             ? std::numeric_limits<int>::max()
             : static_cast<int>(*value);
}

std::int64_t profile_i64_field(const nlohmann::json& object, const char* key,
                               std::int64_t fallback = -1) {
  if (!object.is_object() || !object.contains(key)) return fallback;
  const auto value = parse_nonnegative_integer(object.at(key));
  if (!value) return fallback;
  return *value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
             ? std::numeric_limits<std::int64_t>::max()
             : static_cast<std::int64_t>(*value);
}

std::string profile_string_field(const nlohmann::json& object, const char* key) {
  if (!object.is_object() || !object.contains(key) || !object.at(key).is_string()) return {};
  return object.at(key).get<std::string>();
}

int saturating_profile_add(int value, int delta) {
  value = std::max(0, value);
  delta = std::max(0, delta);
  if (delta > std::numeric_limits<int>::max() - value) return std::numeric_limits<int>::max();
  return value + delta;
}

std::map<std::string, int> parse_failures_by_kind(const nlohmann::json& object) {
  std::map<std::string, int> out;
  if (!object.is_object()) return out;
  for (auto it = object.begin(); it != object.end(); ++it) {
    if (it.key().empty()) continue;
    const auto parsed = parse_nonnegative_integer(it.value());
    if (!parsed) continue;
    const auto count = *parsed > static_cast<std::uint64_t>(std::numeric_limits<int>::max())
                           ? std::numeric_limits<int>::max()
                           : static_cast<int>(*parsed);
    if (count > 0) out[it.key()] = count;
  }
  return out;
}

std::map<std::string, std::int64_t> parse_rtt_by_path(const nlohmann::json& object) {
  std::map<std::string, std::int64_t> out;
  if (!object.is_object()) return out;
  for (auto it = object.begin(); it != object.end(); ++it) {
    if (it.key().empty()) continue;
    const auto parsed = parse_nonnegative_integer(it.value());
    if (!parsed) continue;
    out[it.key()] = *parsed > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                        ? std::numeric_limits<std::int64_t>::max()
                        : static_cast<std::int64_t>(*parsed);
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

std::optional<std::string> save_profile_success(const std::string& fingerprint, const ProfileSuccess& success) {
  const auto storage_path = profile_path();
  auto loaded = load_profile_root(storage_path);
  std::optional<std::string> warning;
  if (loaded.status == ProfileReadStatus::Invalid) {
    std::filesystem::path backup;
    if (auto error = move_file_to_recovery_backup(storage_path, "corrupt", backup)) {
      return "refusing to overwrite malformed network profile: " + *error;
    }
    warning = "malformed network profile preserved at " + backup.string();
  } else if (loaded.status == ProfileReadStatus::Unreadable) {
    return "refusing to overwrite unreadable network profile " + storage_path.string() + ": " + loaded.error;
  }
  auto root = loaded.status == ProfileReadStatus::Loaded ? std::move(loaded.root) : nlohmann::json::object();
  nlohmann::json entry = nlohmann::json::object();
  if (root.contains(fingerprint)) {
    if (root[fingerprint].is_object()) {
      entry = root[fingerprint];
    } else if (!warning) {
      warning = "malformed network profile entry reset for " + fingerprint;
    }
  }
  const int prev = profile_int_field(entry, "success_count");
  const auto prev_path = profile_string_field(entry, "last_path");
  const int prev_streak = profile_int_field(entry, "path_streak", prev_path == success.path ? prev : 0);
  entry["last_path"] = success.path;
  entry["success_count"] = saturating_profile_add(prev, 1);
  entry["path_streak"] = prev_path == success.path ? saturating_profile_add(prev_streak, 1) : 1;

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
      entry["same_port_attempts"] =
          saturating_profile_add(profile_int_field(entry, "same_port_attempts"), stats.same_port_attempts);
      entry["same_port_successes"] =
          saturating_profile_add(profile_int_field(entry, "same_port_successes"), stats.same_port_successes);
      if (stats.same_port_successes > 0) {
        entry["same_port_failure_streak"] = 0;
      } else {
        entry["same_port_failure_streak"] = saturating_profile_add(
            profile_int_field(entry, "same_port_failure_streak"), stats.same_port_failures);
      }
      if (stats.same_port_last_elapsed_ms >= 0) {
        entry["same_port_last_elapsed_ms"] = stats.same_port_last_elapsed_ms;
      }
    }
  }

  root[fingerprint] = std::move(entry);
  if (auto error = atomic_write_text_file(storage_path, root.dump(2) + '\n')) {
    return "failed to save network profile: " + *error;
  }
  return warning;
}

std::string network_fingerprint(const NetworkInterfaceInventory& interfaces) {
  const auto addrs = interfaces.lan_candidate_addresses();
  if (addrs.empty()) return "unknown";
  return addrs.front();
}

std::optional<NetworkProfileEntry> load_profile(const std::string& fingerprint) {
  const auto loaded = load_profile_root(profile_path());
  if (loaded.status != ProfileReadStatus::Loaded) return std::nullopt;
  const auto& root = loaded.root;
  if (!root.contains(fingerprint) || !root[fingerprint].is_object()) return std::nullopt;
  const auto& profile = root[fingerprint];
  NetworkProfileEntry entry;
  entry.last_path = profile_string_field(profile, "last_path");
  entry.success_count = profile_int_field(profile, "success_count");
  entry.path_streak = profile_int_field(profile, "path_streak", entry.last_path.empty() ? 0 : entry.success_count);
  entry.outbound_history.path = profile_string_field(profile, "last_relay_path");
  entry.outbound_history.bind_interface = profile_string_field(profile, "last_relay_interface");
  entry.outbound_history.reason = profile_string_field(profile, "last_relay_reason");
  if (profile.contains("relay_rtt_by_path")) {
    entry.outbound_history.rtt_by_path = parse_rtt_by_path(profile["relay_rtt_by_path"]);
  }
  entry.last_direct_candidate_kind = profile_string_field(profile, "last_direct_candidate_kind");
  entry.last_direct_rtt_ms = profile_i64_field(profile, "last_direct_rtt_ms");
  if (profile.contains("candidate_failures_by_kind")) {
    entry.candidate_failures_by_kind = parse_failures_by_kind(profile["candidate_failures_by_kind"]);
  }
  entry.same_port_attempts = profile_int_field(profile, "same_port_attempts");
  entry.same_port_successes = profile_int_field(profile, "same_port_successes");
  entry.same_port_failure_streak = profile_int_field(profile, "same_port_failure_streak");
  entry.same_port_last_elapsed_ms = profile_i64_field(profile, "same_port_last_elapsed_ms");
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
