#include "platform/user_config.hpp"

#include "core/config.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>

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
  config.network_preset = root.value("network_preset", 0);
  config.advanced_open = root.value("advanced_open", false);
  config.lan_discover = root.value("lan_discover", true);
  config.only_local = root.value("only_local", false);
  config.disable_local = root.value("disable_local", false);
  config.no_direct = root.value("no_direct", false);
  config.udp_probe = root.value("udp_probe", false);
  config.auto_connections = root.value("auto_connections", false);
  config.connections = root.value("connections", 4);
  config.use_gitignore = root.value("use_gitignore", true);
  config.avoid_vpn = root.value("avoid_vpn", false);
  config.manual_ip = root.value("manual_ip", "");
  config.bind_interface = root.value("bind_interface", "");
  config.proxy_url = root.value("proxy_url", "");
  return config;
}

nlohmann::json user_config_to_json(const UserConfig& config) {
  nlohmann::json root;
  if (!config.relay.empty()) root["relay"] = config.relay;
  if (!config.relay_pass.empty()) root["relay_pass"] = config.relay_pass;
  if (!config.last_send_path.empty()) root["last_send_path"] = config.last_send_path;
  if (!config.last_recv_out_dir.empty()) root["last_recv_out_dir"] = config.last_recv_out_dir;
  root["last_mode"] = config.last_mode;
  root["network_preset"] = config.network_preset;
  root["advanced_open"] = config.advanced_open;
  root["lan_discover"] = config.lan_discover;
  root["only_local"] = config.only_local;
  root["disable_local"] = config.disable_local;
  root["no_direct"] = config.no_direct;
  root["udp_probe"] = config.udp_probe;
  root["auto_connections"] = config.auto_connections;
  root["connections"] = config.connections;
  root["use_gitignore"] = config.use_gitignore;
  root["avoid_vpn"] = config.avoid_vpn;
  if (!config.manual_ip.empty()) root["manual_ip"] = config.manual_ip;
  if (!config.bind_interface.empty()) root["bind_interface"] = config.bind_interface;
  if (!config.proxy_url.empty()) root["proxy_url"] = config.proxy_url;
  return root;
}

std::optional<UserConfig> read_config_file(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in) return std::nullopt;
  nlohmann::json root;
  try {
    in >> root;
  } catch (...) {
    return std::nullopt;
  }
  return user_config_from_json(root);
}

void write_config_file(const std::filesystem::path& path, const UserConfig& config) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path);
  if (out) out << user_config_to_json(config).dump(2) << '\n';
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

UserConfig load_user_config() {
  const auto path = user_config_path();
  if (auto loaded = read_config_file(path)) return *loaded;

  const auto legacy = legacy_tui_prefs_path();
  if (legacy != path) {
    if (auto migrated = read_config_file(legacy)) {
      write_config_file(path, *migrated);
      return *migrated;
    }
  }

  return {};
}

void save_user_config(const UserConfig& config) {
  write_config_file(user_config_path(), config);
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

void remember_send_settings(const std::string& relay, const std::optional<std::string>& relay_pass,
                            const std::string& send_path) {
  UserConfig update = load_user_config();
  update.relay = relay;
  if (relay_pass && !relay_pass->empty()) update.relay_pass = *relay_pass;
  update.last_send_path = send_path;
  update.last_mode = 0;
  save_user_config(update);
}

void remember_recv_settings(const std::string& relay, const std::optional<std::string>& relay_pass,
                            const std::string& output_dir) {
  UserConfig update = load_user_config();
  update.relay = relay;
  if (relay_pass && !relay_pass->empty()) update.relay_pass = *relay_pass;
  update.last_recv_out_dir = output_dir;
  update.last_mode = 1;
  save_user_config(update);
}

}  // namespace kiko
