#include "platform.hpp"
#include "profile.hpp"

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
  ProfileRelayPath relay;
  relay.path = "physical";
  relay.bind_interface = "en0";
  relay.reason = "physical_lower_rtt";
  relay.rtt_by_path["default"] = 90;
  relay.rtt_by_path["physical"] = 42;

  save_profile_success("fp-test", "direct", stats, relay);

  auto loaded = load_profile("fp-test");
  assert(loaded);
  assert(loaded->last_path == "direct");
  assert(loaded->success_count == 1);
  assert(loaded->last_relay_path == "physical");
  assert(loaded->last_relay_interface == "en0");
  assert(loaded->last_relay_reason == "physical_lower_rtt");
  assert(loaded->relay_rtt_by_path["default"] == 90);
  assert(loaded->relay_rtt_by_path["physical"] == 42);
  assert(loaded->last_direct_candidate_kind == "lan");
  assert(loaded->last_direct_rtt_ms == 7);
  assert(loaded->candidate_failures_by_kind["public"] == 2);

  std::vector<DirectCandidate> candidates{
      make_direct_candidate(Endpoint{"203.0.113.7", 5000}, "public", 20),
      make_direct_candidate(Endpoint{"192.168.1.10", 5000}, "lan", 90),
  };
  apply_profile_candidate_bias(*loaded, candidates);
  assert(candidates[0].priority == 10);
  assert(candidates[1].priority == 115);

  ConnectivitySnapshot snapshot;
  apply_profile_to_snapshot(*loaded, snapshot);
  assert(snapshot.profile_last_path == "direct");
  assert(snapshot.profile_success_count == 1);
  assert(snapshot.profile_relay_path == "physical");
  assert(snapshot.profile_relay_interface == "en0");
  assert(snapshot.profile_relay_reason == "physical_lower_rtt");
  assert(snapshot.profile_relay_rtt_by_path["default"] == 90);
  assert(snapshot.profile_relay_rtt_by_path["physical"] == 42);
  assert(snapshot.profile_direct_candidate_kind == "lan");
  assert(snapshot.profile_direct_rtt_ms == 7);
  assert(snapshot.profile_candidate_failures_by_kind["public"] == 2);

  fs::remove(path);
  std::cout << "profile_test ok\n";
  return 0;
}
