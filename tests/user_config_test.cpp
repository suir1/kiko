#include "user_config.hpp"

#include "config.hpp"
#include "platform.hpp"

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
  save_user_config(saved);

  const auto loaded = load_user_config();
  if (loaded.relay != saved.relay || loaded.relay_pass != saved.relay_pass ||
      loaded.last_send_path != saved.last_send_path || loaded.last_recv_out_dir != saved.last_recv_out_dir ||
      loaded.last_mode != saved.last_mode) {
    std::cerr << "FAIL: config round-trip mismatch\n";
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
    out << R"({"relay":"legacy:9000","last_mode":0})";
  }
  const auto migrated = load_user_config();
  if (migrated.relay != "legacy:9000" || !fs::exists(config_path)) {
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
