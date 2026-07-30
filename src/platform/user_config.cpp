#include "platform/user_config.hpp"

#include "core/config.hpp"
#include "platform/atomic_file.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <utility>

namespace kiko {
namespace {

std::filesystem::path legacy_tui_prefs_path() {
  if (const char* path = std::getenv("KIKO_TUI_PREFS_PATH")) {
    if (path[0] != '\0') return std::filesystem::path(path);
  }
  if (const char* home = std::getenv("HOME")) {
    return std::filesystem::path(home) / ".config" / "kiko" / "tui.json";
  }
  return std::filesystem::path(".kiko_tui.json");
}

UserConfig user_config_from_json(const nlohmann::json& root) {
  UserConfig config;
  config.relay = root.value("relay", "");
  config.relay_pass = root.value("relay_pass", "");
  config.last_send_path = root.value("last_send_path", "");
  config.last_recv_out_dir = root.value("last_recv_out_dir", "");
  config.last_mode = root.value("last_mode", 0);
  config.network.preset = root.value("network_preset", 0);
  config.network.advanced_open = root.value("advanced_open", false);
  config.network.lan_discover = root.value("lan_discover", true);
  config.network.only_local = root.value("only_local", false);
  config.network.disable_local = root.value("disable_local", false);
  config.network.no_direct = root.value("no_direct", false);
  config.network.udp_probe = root.value("udp_probe", false);
  config.network.auto_connections = root.value("auto_connections", false);
  config.network.connections = root.value("connections", 4);
  config.network.use_gitignore = root.value("use_gitignore", true);
  config.network.avoid_vpn = root.value("avoid_vpn", false);
  config.network.manual_ip = root.value("manual_ip", "");
  config.network.bind_interface = root.value("bind_interface", "");
  config.network.proxy_url = root.value("proxy_url", "");
  return config;
}

nlohmann::json user_config_to_json(const UserConfig& config) {
  nlohmann::json root;
  if (!config.relay.empty()) root["relay"] = config.relay;
  if (!config.relay_pass.empty()) root["relay_pass"] = config.relay_pass;
  if (!config.last_send_path.empty()) root["last_send_path"] = config.last_send_path;
  if (!config.last_recv_out_dir.empty()) root["last_recv_out_dir"] = config.last_recv_out_dir;
  root["last_mode"] = config.last_mode;
  root["network_preset"] = config.network.preset;
  root["advanced_open"] = config.network.advanced_open;
  root["lan_discover"] = config.network.lan_discover;
  root["only_local"] = config.network.only_local;
  root["disable_local"] = config.network.disable_local;
  root["no_direct"] = config.network.no_direct;
  root["udp_probe"] = config.network.udp_probe;
  root["auto_connections"] = config.network.auto_connections;
  root["connections"] = config.network.connections;
  root["use_gitignore"] = config.network.use_gitignore;
  root["avoid_vpn"] = config.network.avoid_vpn;
  if (!config.network.manual_ip.empty()) root["manual_ip"] = config.network.manual_ip;
  if (!config.network.bind_interface.empty()) root["bind_interface"] = config.network.bind_interface;
  if (!config.network.proxy_url.empty()) root["proxy_url"] = config.network.proxy_url;
  return root;
}

enum class ConfigReadStatus { Missing, Loaded, Invalid, Unreadable };

struct ConfigReadResult {
  ConfigReadStatus status = ConfigReadStatus::Missing;
  UserConfig config;
  std::string error;
};

ConfigReadResult read_config_file(const std::filesystem::path& path) {
  std::error_code status_error;
  const auto status = std::filesystem::symlink_status(path, status_error);
  if (status_error == std::errc::no_such_file_or_directory) return {};
  if (!status_error && !std::filesystem::exists(status)) return {};
  if (status_error) return {ConfigReadStatus::Unreadable, {}, status_error.message()};
  if (!std::filesystem::is_regular_file(status)) {
    return {ConfigReadStatus::Unreadable, {}, "path is not a regular file"};
  }

  std::ifstream in(path);
  if (!in) return {ConfigReadStatus::Unreadable, {}, "could not open file"};
  nlohmann::json root;
  try {
    in >> root;
    return {ConfigReadStatus::Loaded, user_config_from_json(root), {}};
  } catch (const std::exception& error) {
    return {ConfigReadStatus::Invalid, {}, error.what()};
  }
}

std::optional<std::string> write_config_file(const std::filesystem::path& path, const UserConfig& config) {
  return atomic_write_text_file(path, user_config_to_json(config).dump(2) + '\n');
}

}  // namespace

std::filesystem::path user_config_path() {
  if (const char* path = std::getenv("KIKO_CONFIG_PATH")) {
    if (path[0] != '\0') return std::filesystem::path(path);
  }
  if (const char* home = std::getenv("HOME")) {
    return std::filesystem::path(home) / ".config" / "kiko" / "config.json";
  }
  return std::filesystem::path(".kiko_config.json");
}

UserConfigLoadResult load_user_config_with_status() {
  const auto path = user_config_path();
  const auto current = read_config_file(path);
  if (current.status == ConfigReadStatus::Loaded) return {current.config, std::nullopt};

  UserConfigLoadResult result;
  bool can_migrate = current.status != ConfigReadStatus::Invalid && current.status != ConfigReadStatus::Unreadable;
  if (current.status == ConfigReadStatus::Invalid) {
    std::filesystem::path backup;
    if (auto error = move_file_to_recovery_backup(path, "corrupt", backup)) {
      result.warning = "user config is malformed and could not be preserved: " + *error;
    } else {
      result.warning = "malformed user config preserved at " + backup.string();
      can_migrate = true;
    }
  } else if (current.status == ConfigReadStatus::Unreadable) {
    result.warning = "could not read user config " + path.string() + ": " + current.error;
  }

  const auto legacy = legacy_tui_prefs_path();
  if (legacy != path) {
    const auto migrated = read_config_file(legacy);
    if (migrated.status == ConfigReadStatus::Loaded) {
      result.config = migrated.config;
      if (can_migrate) {
        if (auto error = write_config_file(path, migrated.config)) {
          result.warning = "loaded legacy config but failed to migrate it: " + *error;
        }
      } else if (!result.warning) {
        result.warning = "loaded legacy config but did not overwrite unreadable user config";
      }
      return result;
    }
  }

  return result;
}

UserConfig load_user_config() { return load_user_config_with_status().config; }

std::optional<std::string> save_user_config(const UserConfig& config) {
  const auto path = user_config_path();
  const auto existing = read_config_file(path);
  if (existing.status == ConfigReadStatus::Invalid) {
    std::filesystem::path backup;
    if (auto error = move_file_to_recovery_backup(path, "corrupt", backup)) {
      return "refusing to overwrite malformed user config: " + *error;
    }
  } else if (existing.status == ConfigReadStatus::Unreadable) {
    return "refusing to overwrite unreadable user config " + path.string() + ": " + existing.error;
  }
  if (auto error = write_config_file(path, config)) {
    return "failed to save user config: " + *error;
  }
  return std::nullopt;
}

std::string resolve_relay_default(const UserConfig& config) {
  if (const char* env = std::getenv("KIKO_RELAY")) {
    if (env[0] != '\0') return env;
  }
  if (!config.relay.empty()) return config.relay;
  return kDefaultRelay;
}

std::optional<std::string> resolve_relay_pass_default(const UserConfig& config) {
  if (const char* env = std::getenv("KIKO_RELAY_PASS")) {
    if (env[0] != '\0') return std::string(env);
  }
  if (!config.relay_pass.empty()) return config.relay_pass;
  return std::nullopt;
}

std::optional<std::string> remember_send_settings(const std::string& relay,
                                                  const std::optional<std::string>& relay_pass,
                                                  const std::string& send_path) {
  auto loaded = load_user_config_with_status();
  UserConfig update = std::move(loaded.config);
  update.relay = relay;
  if (relay_pass && !relay_pass->empty()) update.relay_pass = *relay_pass;
  update.last_send_path = send_path;
  update.last_mode = 0;
  if (auto error = save_user_config(update)) return error;
  return loaded.warning;
}

std::optional<std::string> remember_recv_settings(const std::string& relay,
                                                  const std::optional<std::string>& relay_pass,
                                                  const std::string& output_dir) {
  auto loaded = load_user_config_with_status();
  UserConfig update = std::move(loaded.config);
  update.relay = relay;
  if (relay_pass && !relay_pass->empty()) update.relay_pass = *relay_pass;
  update.last_recv_out_dir = output_dir;
  update.last_mode = 1;
  if (auto error = save_user_config(update)) return error;
  return loaded.warning;
}

}  // namespace kiko
