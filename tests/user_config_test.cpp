#include "platform/user_config.hpp"

#include "core/common.hpp"
#include "core/config.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

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

  const auto root = fs::temp_directory_path() / ("kiko_user_config_test_" + std::to_string(now_ms()));
  const auto config_path = root / "config.json";
  const auto legacy_path = root / "legacy.json";

  fs::remove_all(root);
  fs::create_directories(root);
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
  if (auto error = save_user_config(saved)) {
    std::cerr << "FAIL: config save failed: " << *error << "\n";
    return 1;
  }
#ifndef _WIN32
  {
    std::error_code permission_error;
    const auto permissions = fs::status(config_path, permission_error).permissions();
    const auto exposed = fs::perms::group_all | fs::perms::others_all;
    if (permission_error || (permissions & exposed) != fs::perms::none) {
      std::cerr << "FAIL: config file permissions expose saved relay credentials\n";
      return 1;
    }
  }
#endif

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

  if (auto error = remember_send_settings("send-relay:9000", std::string("send-pass"), "/data")) {
    std::cerr << "FAIL: remember_send_settings failed: " << *error << "\n";
    return 1;
  }
  const auto after_send = load_user_config();
  if (after_send.relay != "send-relay:9000" || after_send.relay_pass != "send-pass" ||
      after_send.last_send_path != "/data" || after_send.last_mode != 0) {
    std::cerr << "FAIL: remember_send_settings\n";
    return 1;
  }

  if (auto error = remember_recv_settings("recv-relay:9000", std::nullopt, "/downloads")) {
    std::cerr << "FAIL: remember_recv_settings failed: " << *error << "\n";
    return 1;
  }
  const auto after_recv = load_user_config();
  if (after_recv.relay != "recv-relay:9000" || after_recv.last_recv_out_dir != "/downloads" ||
      after_recv.last_mode != 1 || after_recv.relay_pass != "send-pass") {
    std::cerr << "FAIL: remember_recv_settings should keep pass and update recv fields\n";
    return 1;
  }

  {
    fs::remove(legacy_path);
    {
      std::ofstream out(config_path, std::ios::trunc);
      out << "{malformed";
    }
    const auto recovered = load_user_config_with_status();
    if (!recovered.warning || recovered.warning->find("preserved") == std::string::npos ||
        fs::exists(config_path)) {
      std::cerr << "FAIL: malformed config was not preserved before recovery\n";
      return 1;
    }
    bool found_backup = false;
    for (const auto& entry : fs::directory_iterator(root)) {
      if (entry.path().filename().string().rfind("config.json.corrupt-", 0) != 0) continue;
      found_backup = true;
      std::ifstream backup(entry.path());
      const std::string contents((std::istreambuf_iterator<char>(backup)), std::istreambuf_iterator<char>());
      if (contents != "{malformed") {
        std::cerr << "FAIL: malformed config backup contents changed\n";
        return 1;
      }
    }
    if (!found_backup) {
      std::cerr << "FAIL: malformed config backup is missing\n";
      return 1;
    }
  }

  {
    const auto directory_target = root / "existing-directory";
    fs::create_directory(directory_target);
    set_env("KIKO_CONFIG_PATH", directory_target.string().c_str());
    const auto error = save_user_config(saved);
    if (!error || error->find("refusing to overwrite unreadable user config") == std::string::npos ||
        !fs::is_directory(directory_target)) {
      std::cerr << "FAIL: config save replaced an existing directory or hid the replacement error\n";
      return 1;
    }
  }

  {
    const auto blocker = root / "blocked-parent";
    {
      std::ofstream out(blocker);
      out << "not a directory";
    }
    const auto blocked_path = blocker / "config.json";
    set_env("KIKO_CONFIG_PATH", blocked_path.string().c_str());
    const auto error = save_user_config(saved);
    if (!error || error->find("refusing to overwrite unreadable user config") == std::string::npos ||
        !fs::is_regular_file(blocker)) {
      std::cerr << "FAIL: config save did not report an unwritable parent\n";
      return 1;
    }
  }

#ifndef _WIN32
  {
    const auto target = root / "symlink-target.json";
    const auto link = root / "symlink-config.json";
    {
      std::ofstream out(target);
      out << "sentinel";
    }
    fs::create_symlink(target, link);
    set_env("KIKO_CONFIG_PATH", link.string().c_str());
    const auto error = save_user_config(saved);
    std::ifstream in(target);
    const std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!error || error->find("refusing to overwrite unreadable user config") == std::string::npos ||
        !fs::is_symlink(link) || contents != "sentinel") {
      std::cerr << "FAIL: config save followed or replaced a symbolic link\n";
      return 1;
    }
  }

  {
    const auto locked_root = root / "locked-recovery";
    const auto locked_config = locked_root / "config.json";
    fs::create_directory(locked_root);
    {
      std::ofstream out(locked_config);
      out << "{malformed";
    }
    {
      std::ofstream out(legacy_path);
      out << R"({"relay":"legacy-after-backup-failure:9000"})";
    }
    fs::permissions(locked_root, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);
    set_env("KIKO_CONFIG_PATH", locked_config.string().c_str());
    const auto recovered = load_user_config_with_status();
    const auto save_error = save_user_config(saved);
    fs::permissions(locked_root, fs::perms::owner_all, fs::perm_options::replace);

    std::ifstream in(locked_config);
    const std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (recovered.config.relay != "legacy-after-backup-failure:9000" || !recovered.warning ||
        recovered.warning->find("could not be preserved") == std::string::npos || !save_error ||
        save_error->find("refusing to overwrite malformed user config") == std::string::npos ||
        contents != "{malformed") {
      std::cerr << "FAIL: failed config backup allowed the malformed file to be overwritten\n";
      return 1;
    }
  }
#endif

  for (const auto& entry : fs::directory_iterator(root)) {
    if (entry.path().filename().string().find(".tmp-") != std::string::npos) {
      std::cerr << "FAIL: config save left a temporary file behind\n";
      return 1;
    }
  }

  fs::remove_all(root);
  unset_env("KIKO_CONFIG_PATH");
  unset_env("KIKO_TUI_PREFS_PATH");

  std::cout << "user_config_test ok\n";
  return 0;
}
