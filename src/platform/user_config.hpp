#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace kiko {

struct NetworkPreferences {
  int preset = 0;
  bool advanced_open = false;
  bool lan_discover = true;
  bool only_local = false;
  bool disable_local = false;
  bool no_direct = false;
  bool udp_probe = false;
  bool auto_connections = false;
  int connections = 4;
  bool use_gitignore = true;
  bool avoid_vpn = false;
  std::string manual_ip;
  std::string bind_interface;
  std::string proxy_url;
};

// Shared CLI/TUI preferences at ~/.config/kiko/config.json (override: KIKO_CONFIG_PATH).
struct UserConfig {
  std::string relay;
  std::string relay_pass;
  std::string last_send_path;
  std::string last_recv_out_dir;
  int last_mode = 0;
  NetworkPreferences network;
};

[[nodiscard]] std::filesystem::path user_config_path();

// Loads config.json; migrates legacy tui.json when present.
[[nodiscard]] UserConfig load_user_config();

void save_user_config(const UserConfig& config);

// Priority: KIKO_RELAY env → saved relay → compile-time default.
[[nodiscard]] std::string resolve_relay_default(const UserConfig& config);

// Priority: KIKO_RELAY_PASS env → saved relay_pass → nullopt.
[[nodiscard]] std::optional<std::string> resolve_relay_pass_default(const UserConfig& config);

void remember_send_settings(const std::string& relay, const std::optional<std::string>& relay_pass,
                            const std::string& send_path);
void remember_recv_settings(const std::string& relay, const std::optional<std::string>& relay_pass,
                            const std::string& output_dir);

}  // namespace kiko
