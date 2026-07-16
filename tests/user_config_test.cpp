#include "platform/user_config.hpp"

#include "core/config.hpp"
#include "platform/platform.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

namespace {

void set_env(const char* key, const char* value) {
#if defined(_WIN32)
  _putenv_s(key, value);
#else
  setenv(key, value, 1);
#endif
}

void unset_env(const char* key) {
#if defined(_WIN32)
  _putenv_s(key, "");
#else
  unsetenv(key);
#endif
}

bool same_network(const kiko::NetworkPreferences& a, const kiko::NetworkPreferences& b) {
  return a.preset == b.preset && a.advanced_open == b.advanced_open && a.lan_discover == b.lan_discover &&
         a.only_local == b.only_local && a.disable_local == b.disable_local && a.no_direct == b.no_direct &&
         a.udp_probe == b.udp_probe && a.auto_connections == b.auto_connections &&
         a.connections == b.connections && a.use_gitignore == b.use_gitignore && a.avoid_vpn == b.avoid_vpn &&
         a.manual_ip == b.manual_ip && a.bind_interface == b.bind_interface && a.proxy_url == b.proxy_url;
}

}  // namespace

int main() {
  using namespace kiko;

  const auto config_path =
      fs::temp_directory_path() / ("kiko_user_config_test_" + std::to_string(process_id()) + ".json");
  const auto legacy_path = config_path.parent_path() / (config_path.stem().string() + "_legacy.json");

  fs::remove(config_path);
  fs::remove(legacy_path);
  set_env("KIKO_CONFIG_PATH", config_path.string().c_str());
  set_env("KIKO_TUI_PREFS_PATH", legacy_path.string().c_str());
  unset_env("KIKO_RELAY");
  unset_env("KIKO_RELAY_PASS");

  if (!load_user_config().relay.empty()) {
    std::cerr << "FAIL: expected empty config\n";
    return 1;
  }

  UserConfig saved;
  saved.relay = "relay.example.com:9000";
  saved.relay_pass = "secret";
  saved.last_send_path = "/tmp/kiko";
  saved.last_recv_out_dir = "/tmp/out";
  saved.last_mode = 1;
  saved.network = {3, true, false, true, false, true, true, true, 8, false, true,
                   "192.0.2.10", "en0", "socks5://127.0.0.1:1080"};
  save_user_config(saved);

  const auto loaded = load_user_config();
  if (loaded.relay != saved.relay || loaded.relay_pass != saved.relay_pass ||
      loaded.last_send_path != saved.last_send_path || loaded.last_recv_out_dir != saved.last_recv_out_dir ||
      loaded.last_mode != saved.last_mode || !same_network(loaded.network, saved.network)) {
    std::cerr << "FAIL: config round-trip mismatch\n";
    return 1;
  }

  nlohmann::json persisted;
  {
    std::ifstream in(config_path);
    in >> persisted;
  }
  if (!persisted.contains("network_preset") || !persisted.contains("no_direct") ||
      !persisted.contains("connections") || persisted.contains("network")) {
    std::cerr << "FAIL: network preferences changed the persisted JSON layout\n";
    return 1;
  }

  if (resolve_relay_default(loaded) != saved.relay) {
    std::cerr << "FAIL: resolve_relay_default should use saved relay\n";
    return 1;
  }

  set_env("KIKO_RELAY", "env-relay:9001");
  if (resolve_relay_default(loaded) != "env-relay:9001") {
    std::cerr << "FAIL: env should override saved relay\n";
    return 1;
  }
  unset_env("KIKO_RELAY");

  set_env("KIKO_RELAY_PASS", "env-pass");
  const auto pass = resolve_relay_pass_default(loaded);
  if (!pass || *pass != "env-pass") {
    std::cerr << "FAIL: env should override saved relay pass\n";
    return 1;
  }
  unset_env("KIKO_RELAY_PASS");

  fs::remove(config_path);
  {
    std::ofstream out(legacy_path);
    out << R"({"relay":"legacy:9000","last_mode":0,"no_direct":true,"connections":6})";
  }
  const auto migrated = load_user_config();
  if (migrated.relay != "legacy:9000" || !migrated.network.no_direct || migrated.network.connections != 6 ||
      !fs::exists(config_path)) {
    std::cerr << "FAIL: legacy tui.json migration\n";
    return 1;
  }

  remember_send_settings("send-relay:9000", std::string("send-pass"), "/data");
  const auto after_send = load_user_config();
  if (after_send.relay != "send-relay:9000" || after_send.relay_pass != "send-pass" ||
      after_send.last_send_path != "/data" || after_send.last_mode != 0) {
    std::cerr << "FAIL: remember_send_settings\n";
    return 1;
  }

  remember_recv_settings("recv-relay:9000", std::nullopt, "/downloads");
  const auto after_recv = load_user_config();
  if (after_recv.relay != "recv-relay:9000" || after_recv.last_recv_out_dir != "/downloads" ||
      after_recv.last_mode != 1 || after_recv.relay_pass != "send-pass") {
    std::cerr << "FAIL: remember_recv_settings should keep pass and update recv fields\n";
    return 1;
  }

  fs::remove(config_path);
  fs::remove(legacy_path);
  unset_env("KIKO_CONFIG_PATH");
  unset_env("KIKO_TUI_PREFS_PATH");

  std::cout << "user_config_test ok\n";
  return 0;
}
