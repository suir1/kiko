#include "platform.hpp"
#include "direct_session.hpp"
#include "profile.hpp"
#include "progress.hpp"
#include "route_planner.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <vector>

namespace {

using namespace kiko;

StunProbeResult stun(StunNatClass type) {
  StunProbeResult result;
  result.ok = true;
  result.nat_class = type;
  result.mapped = Endpoint{"203.0.113.10", 40000};
  result.mapped_alt =
      Endpoint{"203.0.113.10", static_cast<std::uint16_t>(type == StunNatClass::Symmetric ? 40001 : 40000)};
  return result;
}

void assert_ms(std::chrono::milliseconds actual, std::int64_t expected, const char* label) {
  if (actual.count() != expected) {
    std::cerr << "FAIL: " << label << " expected " << expected << "ms, got " << actual.count() << "ms\n";
    std::abort();
  }
}

void assert_reason(const RoutePlan& plan, const std::string& expected) {
  if (plan.reason != expected) {
    std::cerr << "FAIL: expected route reason '" << expected << "', got '" << plan.reason << "'\n";
    std::abort();
  }
}

struct RecordingReporter : ProgressReporter {
  std::vector<std::string> statuses;

  void status(const std::string& message) override { statuses.push_back(message); }
};

}  // namespace

int main() {
  using namespace kiko;
  namespace fs = std::filesystem;

  const auto profile_path =
      fs::temp_directory_path() / ("kiko_route_scenarios_" + std::to_string(process_id()) + ".json");
  fs::remove(profile_path);
#ifdef _WIN32
  _putenv_s("KIKO_PROFILE_PATH", profile_path.string().c_str());
#else
  setenv("KIKO_PROFILE_PATH", profile_path.string().c_str(), 1);
#endif

  RuleScheduler rules;
  auto clear_profile = [&]() { fs::remove(profile_path); };

  {
    ConnectivitySnapshot snapshot;
    const auto plan = rules.plan(snapshot, std::nullopt, true, 4);
    assert(plan.skip_direct);
    assert(!plan.udp_punch_enabled);
    assert_ms(plan.direct_timeout, 0, "no-direct timeout");
    assert_reason(plan, "no_direct");
  }

  {
    ConnectivitySnapshot snapshot;
    auto plan = rules.plan(snapshot, std::nullopt, false, 4);
    Message peer{"peer", {{"peer_no_direct", "1"}}};
    apply_peer_direct_policy(plan, peer);
    assert(plan.skip_direct);
    assert(!plan.udp_punch_enabled);
    assert_ms(plan.direct_timeout, 0, "peer-no-direct timeout");
    assert_ms(plan.direct_connect, 0, "peer-no-direct connect timeout");
    assert_reason(plan, "peer_no_direct");
  }

  {
    auto listener = TcpListener::bind(Endpoint{"127.0.0.1", 0});
    Message peer{"peer",
                 {{"peer_local_candidates", "127.0.0.1"},
                  {"peer_listen_host", "127.0.0.1"},
                  {"peer_listen_port", std::to_string(listener.local_endpoint().port)}}};
    RoutePlan plan;
    plan.skip_direct = true;
    plan.direct_timeout = std::chrono::milliseconds(0);
    plan.direct_connect = std::chrono::milliseconds(0);

    AdaptivePuncher puncher;
    const auto direct = attempt_direct(Role::Receiver, listener, peer, {}, puncher, NatProfile{}, NatProfile{}, plan,
                                       "skip-direct-room");
    assert(!direct);
    assert(puncher.observations().empty());
  }

  {
    ConnectivitySnapshot snapshot;
    const auto plan = rules.plan(snapshot, stun(StunNatClass::Open), false, 8);
    assert(!plan.skip_direct);
    assert_ms(plan.direct_timeout, 3500, "open NAT direct window");
    assert_ms(plan.direct_connect, 450, "open NAT connect timeout");
    assert(plan.connections == 8);
    assert_reason(plan, "stun_open");
  }

  {
    ConnectivitySnapshot snapshot;
    const auto plan = rules.plan(snapshot, stun(StunNatClass::Cone), false, 4);
    assert(!plan.skip_direct);
    assert(!plan.udp_punch_enabled);
    assert_ms(plan.direct_timeout, 2500, "cone NAT direct window");
    assert_reason(plan, "stun_cone_direct_probe");
  }

  {
    ConnectivitySnapshot snapshot;
    const auto plan = rules.plan(snapshot, stun(StunNatClass::Symmetric), false, 4);
    assert(!plan.skip_direct);
    assert(!plan.udp_punch_enabled);
    assert_ms(plan.direct_timeout, 500, "symmetric NAT direct window");
    assert_ms(plan.direct_connect, 220, "symmetric NAT connect timeout");
    assert_reason(plan, "stun_symmetric_short_direct");
  }

  {
    ConnectivitySnapshot snapshot;
    snapshot.self_nat = NatType::BehindNat;
    snapshot.peer_nat = NatType::BehindNat;
    const auto plan = rules.plan(snapshot, std::nullopt, false, 4);
    assert(!plan.skip_direct);
    assert_ms(plan.direct_timeout, 500, "double NAT direct window");
    assert_ms(plan.direct_connect, 220, "double NAT connect timeout");
    assert_reason(plan, "double_nat_short_punch");
  }

  {
    ConnectivitySnapshot snapshot;
    snapshot.vpn_detected = true;
    snapshot.lan_discovered_count = 2;
    const auto plan = rules.plan(snapshot, std::nullopt, false, 4);
    assert(!plan.skip_direct);
    assert_ms(plan.direct_timeout, 1000, "VPN LAN direct window");
    assert_ms(plan.direct_connect, 250, "VPN LAN connect timeout");
    assert_reason(plan, "vpn_lan_short_direct");
  }

  {
    ConnectivitySnapshot snapshot;
    snapshot.only_local = true;
    const auto plan = rules.plan(snapshot, std::nullopt, false, 4);
    assert(!plan.skip_direct);
    assert_reason(plan, "only_local");
  }

  {
    clear_profile();
    ConnectivitySnapshot snapshot;
    snapshot.relays.push_back(RelayProbeEntry{"external", "relay.example:9000", 25, true});
    const auto plan = build_route_plan(false, snapshot, std::nullopt, 4);
    assert(!plan.skip_direct);
    assert_ms(plan.direct_timeout, 2500, "default direct window without profile");
    assert_reason(plan, "default");
  }

  {
    clear_profile();
    const auto fingerprint = network_fingerprint();
    PunchStats stats;
    stats.attempted = true;
    stats.direct_ok = true;
    stats.successful_candidate_kind = "lan";
    stats.successful_candidate_priority = 90;
    stats.successful_elapsed_ms = 12;
    stats.candidate_failures_by_kind["public"] = 3;
    save_profile_success(fingerprint, "direct", stats);

    ConnectivitySnapshot snapshot;
    const auto plan = build_route_plan(false, snapshot, std::nullopt, 4);
    assert(!plan.skip_direct);
    assert_ms(plan.direct_timeout, 2500, "direct success profile keeps default direct window");
    assert_reason(plan, "default");
    if (plan.direct_candidate_order.empty() || plan.direct_candidate_order.front() != "lan") {
      std::cerr << "FAIL: direct success profile did not prioritize prior successful candidate kind\n";
      return 1;
    }
  }

  {
    clear_profile();
    const auto fingerprint = network_fingerprint();
    PunchStats stats;
    stats.attempted = true;
    stats.direct_ok = false;
    stats.candidate_failures_by_kind["public"] = 1;
    stats.candidate_failures_by_kind["public-same-port"] = 1;
    save_profile_success(fingerprint, "relay", stats);

    ConnectivitySnapshot snapshot;
    const auto plan = build_route_plan(false, snapshot, std::nullopt, 4);
    assert(!plan.skip_direct);
    assert_ms(plan.direct_timeout, 900, "public failure profile direct window");
    assert_ms(plan.direct_connect, 250, "public failure profile connect timeout");
    assert_reason(plan, "profile_public_failures_short_direct");
    if (plan.direct_candidate_order.empty() || plan.direct_candidate_order.front() != "lan" ||
        plan.direct_candidate_order.back() != "public") {
      std::cerr << "FAIL: public failure profile did not prefer local candidates before public\n";
      return 1;
    }
  }

  {
    clear_profile();
    const auto fingerprint = network_fingerprint();
    save_profile_success(fingerprint, "relay");
    save_profile_success(fingerprint, "relay");

    ConnectivitySnapshot snapshot;
    const auto plan = build_route_plan(false, snapshot, std::nullopt, 4);
    assert(!plan.skip_direct);
    assert_ms(plan.direct_timeout, 600, "relay history direct window");
    assert_ms(plan.direct_connect, 220, "relay history connect timeout");
    assert_reason(plan, "profile_relay_history_short_direct");
  }

  {
    RoutePlan plan;
    plan.direct_timeout = std::chrono::milliseconds(600);
    plan.direct_connect = std::chrono::milliseconds(180);
    plan.direct_candidate_order = {"public", "lan"};

    std::vector<DirectCandidate> candidates{
        make_direct_candidate(Endpoint{"192.168.1.10", 5000}, "lan", 90),
        make_direct_candidate(Endpoint{"203.0.113.7", 5000}, "public", 20),
    };
    NatProfile open;
    open.type = NatType::Open;
    AdaptivePuncher puncher;
    PunchPlan punch;
    apply_route_plan_to_adaptive(plan, Role::Receiver, puncher, candidates, open, NatProfile{}, punch);
    assert_ms(punch.total_timeout, 600, "planned direct window");
    assert_ms(punch.connect_timeout, 180, "planned connect timeout");
    if (punch.candidates.empty() || punch.candidates.front().kind != "public") {
      std::cerr << "FAIL: planned direct candidate order was not applied\n";
      return 1;
    }
  }

  {
    auto listener = TcpListener::bind(Endpoint{"127.0.0.1", 0});
    Message peer{"peer",
                 {{"peer_listen_host", "127.0.0.1"},
                  {"peer_listen_port", "1"},
                  {"peer_public_host", "203.0.113.7"},
                  {"peer_public_port", "5000"}}};
    RoutePlan plan;
    plan.direct_timeout = std::chrono::milliseconds(20);
    plan.direct_connect = std::chrono::milliseconds(5);

    AdaptivePuncher puncher;
    RecordingReporter reporter;
    const auto direct = attempt_direct(Role::Receiver, listener, peer, {}, puncher, NatProfile{}, NatProfile{}, plan,
                                       "observable-direct-room", ConnectOptions{}, &reporter);
    assert(!direct);
    bool saw_plan = false;
    for (const auto& status : reporter.statuses) {
      if (status.find("direct plan: timeout=20ms connect=5ms") != std::string::npos &&
          status.find("listen@127.0.0.1:1") != std::string::npos &&
          status.find("public@203.0.113.7:5000") != std::string::npos) {
        saw_plan = true;
      }
    }
    if (!saw_plan) {
      std::cerr << "FAIL: direct attempt did not emit observable candidate plan\n";
      return 1;
    }
  }

  {
    auto listener = TcpListener::bind(Endpoint{"127.0.0.1", 0});
    Message peer{"peer", {{"peer_public_host", "127.0.0.1"}, {"peer_public_port", "1"}}};
    RoutePlan plan;
    plan.direct_timeout = std::chrono::milliseconds(2500);
    plan.direct_connect = std::chrono::milliseconds(450);

    AdaptivePuncher puncher;
    RecordingReporter reporter;
    const auto direct = attempt_direct(Role::Receiver, listener, peer, {}, puncher, NatProfile{}, NatProfile{}, plan,
                                       "public-only-direct-room", ConnectOptions{}, &reporter);
    assert(!direct);
    bool saw_guard = false;
    for (const auto& status : reporter.statuses) {
      if (status.find("direct plan: timeout=500ms connect=220ms") != std::string::npos &&
          status.find("public@127.0.0.1:1") != std::string::npos &&
          status.find("relay_fallback_guard") != std::string::npos) {
        saw_guard = true;
      }
    }
    if (!saw_guard) {
      std::cerr << "FAIL: public-only direct plan did not shorten for relay fallback\n";
      return 1;
    }
  }

  fs::remove(profile_path);
  std::cout << "PASS: route scenario decisions\n";
  return 0;
}
