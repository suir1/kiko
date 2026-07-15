#include "platform/platform.hpp"
#include "connect/profile.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>

int main() {
  using namespace kiko;
  namespace fs = std::filesystem;

  const auto path = fs::temp_directory_path() / ("kiko_profile_test_" + std::to_string(process_id()) + ".json");
  fs::remove(path);
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

  save_profile_success("fp-test", ProfileSuccess{"direct", stats, relay});

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

  save_profile_success("fp-test", ProfileSuccess{"direct", stats, relay});
  loaded = load_profile("fp-test");
  assert(loaded);
  assert(loaded->last_path == "direct");
  assert(loaded->success_count == 2);
  assert(loaded->path_streak == 2);

  save_profile_success("fp-test", ProfileSuccess{"relay", std::nullopt, relay});
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
  save_profile_success("fp-same-port", ProfileSuccess{"relay", same_port_fail, std::nullopt});
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
  save_profile_success("fp-same-port", ProfileSuccess{"direct", same_port_success, std::nullopt});
  same_port_loaded = load_profile("fp-same-port");
  assert(same_port_loaded);
  assert(same_port_loaded->same_port_attempts == 5);
  assert(same_port_loaded->same_port_successes == 1);
  assert(same_port_loaded->same_port_failure_streak == 0);
  assert(same_port_loaded->same_port_last_elapsed_ms == 37);

  fs::remove(path);
  std::cout << "profile_test ok\n";
  return 0;
}
