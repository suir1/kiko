#include "connect/profile.hpp"
#include "core/common.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>

int main() {
  using namespace kiko;
  namespace fs = std::filesystem;

  const auto root = fs::temp_directory_path() / ("kiko_profile_test_" + std::to_string(now_ms()));
  const auto path = root / "profile.json";
  fs::remove_all(root);
  fs::create_directories(root);
#ifdef _WIN32
  _putenv_s("KIKO_PROFILE_PATH", path.string().c_str());
#else
  setenv("KIKO_PROFILE_PATH", path.string().c_str(), 1);
#endif

  PunchStats stats;
  stats.attempted = true;
  stats.direct_ok = true;
  stats.successful_candidate_kind = "lan";
  stats.successful_candidate_priority = 90;
  stats.successful_elapsed_ms = 7;
  stats.candidate_failures_by_kind["public"] = 2;
  OutboundHistory relay;
  relay.path = "physical";
  relay.bind_interface = "en0";
  relay.reason = "physical_lower_rtt";
  relay.rtt_by_path["default"] = 90;
  relay.rtt_by_path["physical"] = 42;

  assert(!save_profile_success("fp-test", ProfileSuccess{"direct", stats, relay}));

  auto loaded = load_profile("fp-test");
  assert(loaded);
  assert(loaded->last_path == "direct");
  assert(loaded->success_count == 1);
  assert(loaded->path_streak == 1);
  assert(loaded->outbound_history.path == "physical");
  assert(loaded->outbound_history.bind_interface == "en0");
  assert(loaded->outbound_history.reason == "physical_lower_rtt");
  assert(loaded->outbound_history.rtt_by_path["default"] == 90);
  assert(loaded->outbound_history.rtt_by_path["physical"] == 42);
  assert(loaded->last_direct_candidate_kind == "lan");
  assert(loaded->last_direct_rtt_ms == 7);
  assert(loaded->candidate_failures_by_kind["public"] == 2);
  auto outbound_history = outbound_history_from_profile(*loaded);
  assert(outbound_history);
  assert(outbound_history->path == "physical");
  assert(outbound_history->bind_interface == "en0");
  assert(outbound_history->reason == "physical_lower_rtt");
  assert(outbound_history->rtt_by_path["default"] == 90);
  assert(outbound_history->rtt_by_path["physical"] == 42);

  ConnectivitySnapshot snapshot;
  snapshot.profile = *loaded;
  assert(snapshot.profile.last_path == "direct");
  assert(snapshot.profile.success_count == 1);
  assert(snapshot.profile.path_streak == 1);
  assert(snapshot.profile.outbound_history.path == "physical");
  assert(snapshot.profile.outbound_history.bind_interface == "en0");
  assert(snapshot.profile.outbound_history.reason == "physical_lower_rtt");
  assert(snapshot.profile.outbound_history.rtt_by_path["default"] == 90);
  assert(snapshot.profile.outbound_history.rtt_by_path["physical"] == 42);
  assert(snapshot.profile.last_direct_candidate_kind == "lan");
  assert(snapshot.profile.last_direct_rtt_ms == 7);
  assert(snapshot.profile.candidate_failures_by_kind["public"] == 2);

  assert(!save_profile_success("fp-test", ProfileSuccess{"direct", stats, relay}));
  loaded = load_profile("fp-test");
  assert(loaded);
  assert(loaded->last_path == "direct");
  assert(loaded->success_count == 2);
  assert(loaded->path_streak == 2);

  assert(!save_profile_success("fp-test", ProfileSuccess{"relay", std::nullopt, relay}));
  loaded = load_profile("fp-test");
  assert(loaded);
  assert(loaded->last_path == "relay");
  assert(loaded->success_count == 3);
  assert(loaded->path_streak == 1);

  PunchStats same_port_fail;
  same_port_fail.attempted = true;
  same_port_fail.direct_ok = false;
  same_port_fail.same_port_attempts = 4;
  same_port_fail.same_port_failures = 4;
  same_port_fail.same_port_last_elapsed_ms = 91;
  assert(!save_profile_success("fp-same-port", ProfileSuccess{"relay", same_port_fail, std::nullopt}));
  auto same_port_loaded = load_profile("fp-same-port");
  assert(same_port_loaded);
  assert(same_port_loaded->same_port_attempts == 4);
  assert(same_port_loaded->same_port_successes == 0);
  assert(same_port_loaded->same_port_failure_streak == 4);
  assert(same_port_loaded->same_port_last_elapsed_ms == 91);

  PunchStats same_port_success;
  same_port_success.attempted = true;
  same_port_success.direct_ok = true;
  same_port_success.same_port_attempts = 1;
  same_port_success.same_port_successes = 1;
  same_port_success.same_port_last_elapsed_ms = 37;
  assert(!save_profile_success("fp-same-port", ProfileSuccess{"direct", same_port_success, std::nullopt}));
  same_port_loaded = load_profile("fp-same-port");
  assert(same_port_loaded);
  assert(same_port_loaded->same_port_attempts == 5);
  assert(same_port_loaded->same_port_successes == 1);
  assert(same_port_loaded->same_port_failure_streak == 0);
  assert(same_port_loaded->same_port_last_elapsed_ms == 37);

  {
    std::ofstream out(path, std::ios::trunc);
    out << R"({
      "fp-primitive": "bad",
      "fp-fields": {
        "last_path": 7,
        "success_count": "many",
        "path_streak": 18446744073709551615,
        "last_direct_rtt_ms": "fast",
        "relay_rtt_by_path": {"bad": "x", "huge": 18446744073709551615},
        "candidate_failures_by_kind": {"bad": "x", "huge": 18446744073709551615},
        "same_port_attempts": 18446744073709551615
      },
      "fp-saturated": {
        "last_path": "direct",
        "success_count": 2147483647,
        "path_streak": 2147483647,
        "same_port_attempts": 2147483647,
        "same_port_successes": 2147483647,
        "same_port_failure_streak": 2147483647
      }
    })";
  }

  if (load_profile("fp-primitive")) {
    std::cerr << "FAIL: primitive profile entry was treated as a valid profile\n";
    return 1;
  }
  const auto fields = load_profile("fp-fields");
  if (!fields || !fields->last_path.empty() || fields->success_count != 0 ||
      fields->path_streak != std::numeric_limits<int>::max() || fields->last_direct_rtt_ms != -1 ||
      fields->outbound_history.rtt_by_path.at("huge") != std::numeric_limits<std::int64_t>::max() ||
      fields->candidate_failures_by_kind.at("huge") != std::numeric_limits<int>::max() ||
      fields->same_port_attempts != std::numeric_limits<int>::max()) {
    std::cerr << "FAIL: malformed profile fields were not safely defaulted or clamped\n";
    return 1;
  }

  const auto primitive_warning =
      save_profile_success("fp-primitive", ProfileSuccess{"direct", stats, relay});
  const auto primitive_recovered = load_profile("fp-primitive");
  if (!primitive_warning || primitive_warning->find("entry reset") == std::string::npos ||
      !primitive_recovered || primitive_recovered->success_count != 1) {
    std::cerr << "FAIL: primitive profile entry was not recovered safely\n";
    return 1;
  }

  assert(!save_profile_success("fp-saturated", ProfileSuccess{"direct", same_port_fail, std::nullopt}));
  const auto saturated = load_profile("fp-saturated");
  if (!saturated || saturated->success_count != std::numeric_limits<int>::max() ||
      saturated->path_streak != std::numeric_limits<int>::max() ||
      saturated->same_port_attempts != std::numeric_limits<int>::max() ||
      saturated->same_port_successes != std::numeric_limits<int>::max() ||
      saturated->same_port_failure_streak != std::numeric_limits<int>::max()) {
    std::cerr << "FAIL: profile counters overflowed instead of saturating\n";
    return 1;
  }

  {
    std::ofstream out(path, std::ios::trunc);
    out << "{malformed";
  }
  const auto recovery_warning = save_profile_success("fp-recovered", ProfileSuccess{"direct", stats, relay});
  if (!recovery_warning || recovery_warning->find("preserved") == std::string::npos) {
    std::cerr << "FAIL: malformed profile was overwritten without a recovery warning\n";
    return 1;
  }
  bool found_profile_backup = false;
  for (const auto& entry : fs::directory_iterator(root)) {
    if (entry.path().filename().string().rfind("profile.json.corrupt-", 0) != 0) continue;
    found_profile_backup = true;
    std::ifstream backup(entry.path());
    const std::string contents((std::istreambuf_iterator<char>(backup)), std::istreambuf_iterator<char>());
    if (contents != "{malformed") {
      std::cerr << "FAIL: malformed profile backup contents changed\n";
      return 1;
    }
  }
  if (!found_profile_backup || !load_profile("fp-recovered")) {
    std::cerr << "FAIL: malformed profile recovery did not produce a valid replacement\n";
    return 1;
  }

  const auto blocker = root / "blocked-parent";
  {
    std::ofstream out(blocker);
    out << "not a directory";
  }
  const auto blocked_path = blocker / "profile.json";
#ifdef _WIN32
  _putenv_s("KIKO_PROFILE_PATH", blocked_path.string().c_str());
#else
  setenv("KIKO_PROFILE_PATH", blocked_path.string().c_str(), 1);
#endif
  const auto save_error = save_profile_success("fp-test", ProfileSuccess{"direct", stats, relay});
  const bool expected_save_error =
      save_error && (save_error->find("refusing to overwrite unreadable network profile") != std::string::npos ||
                     save_error->find("failed to save network profile") != std::string::npos);
  if (!expected_save_error || !fs::is_regular_file(blocker)) {
    std::cerr << "FAIL: profile save did not report an unwritable parent\n";
    return 1;
  }

#ifndef _WIN32
  {
    const auto target = root / "symlink-target.json";
    const auto link = root / "symlink-profile.json";
    {
      std::ofstream out(target);
      out << "sentinel";
    }
    fs::create_symlink(target, link);
    setenv("KIKO_PROFILE_PATH", link.string().c_str(), 1);
    const auto error = save_profile_success("fp-test", ProfileSuccess{"direct", stats, relay});
    std::ifstream in(target);
    const std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!error || error->find("refusing to overwrite unreadable network profile") == std::string::npos ||
        !fs::is_symlink(link) || contents != "sentinel") {
      std::cerr << "FAIL: profile save followed or replaced a symbolic link\n";
      return 1;
    }
  }

  {
    const auto locked_root = root / "locked-recovery";
    const auto locked_profile = locked_root / "profile.json";
    fs::create_directory(locked_root);
    {
      std::ofstream out(locked_profile);
      out << "{malformed";
    }
    fs::permissions(locked_root, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);
    setenv("KIKO_PROFILE_PATH", locked_profile.string().c_str(), 1);
    const auto error = save_profile_success("fp-test", ProfileSuccess{"direct", stats, relay});
    fs::permissions(locked_root, fs::perms::owner_all, fs::perm_options::replace);

    std::ifstream in(locked_profile);
    const std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!error || error->find("refusing to overwrite malformed network profile") == std::string::npos ||
        contents != "{malformed") {
      std::cerr << "FAIL: failed profile backup allowed the malformed file to be overwritten\n";
      return 1;
    }
  }
#endif

  for (const auto& entry : fs::directory_iterator(root)) {
    if (entry.path().filename().string().find(".tmp-") != std::string::npos) {
      std::cerr << "FAIL: profile save left a temporary file behind\n";
      return 1;
    }
  }

  fs::remove_all(root);
  std::cout << "profile_test ok\n";
  return 0;
}
