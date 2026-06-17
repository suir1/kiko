#include "platform.hpp"
#include "profile.hpp"
#include "route_planner.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>

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

}  // namespace

int main() {
  using namespace kiko;
  namespace fs = std::filesystem;

  const auto profile_path =
      fs::temp_directory_path() / ("kiko_route_scenarios_" + std::to_string(process_id()) + ".json");
  fs::remove(profile_path);
  setenv("KIKO_PROFILE_PATH", profile_path.string().c_str(), 1);

  RuleScheduler rules;

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
    assert_ms(plan.direct_timeout, 700, "symmetric NAT direct window");
    assert_ms(plan.direct_connect, 220, "symmetric NAT connect timeout");
    assert_reason(plan, "stun_symmetric_short_direct");
  }

  {
    ConnectivitySnapshot snapshot;
    snapshot.self_nat = NatType::BehindNat;
    snapshot.peer_nat = NatType::BehindNat;
    const auto plan = rules.plan(snapshot, std::nullopt, false, 4);
    assert(!plan.skip_direct);
    assert_ms(plan.direct_timeout, 800, "double NAT direct window");
    assert_ms(plan.direct_connect, 250, "double NAT connect timeout");
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
    ConnectivitySnapshot snapshot;
    snapshot.relays.push_back(RelayProbeEntry{"external", "relay.example:9000", 25, true});
    const auto plan = build_route_plan(false, snapshot, std::nullopt, 4);
    assert(!plan.skip_direct);
    assert_ms(plan.direct_timeout, 2500, "default direct window without profile");
    assert_reason(plan, "default");
  }

  {
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

  fs::remove(profile_path);
  std::cout << "PASS: route scenario decisions\n";
  return 0;
}
