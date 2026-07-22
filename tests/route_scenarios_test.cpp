#include "connect/direct_session.hpp"
#include "core/progress.hpp"
#include "connect/route_planner.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>
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

  {
    ConnectivitySnapshot snapshot;
    const auto plan = build_route_plan(true, snapshot, std::nullopt, 4);
    assert(plan.skip_direct);
    assert(!plan.udp_punch_enabled);
    assert_ms(plan.direct_timeout, 0, "no-direct timeout");
    assert_reason(plan, "no_direct");
  }

  {
    ConnectivitySnapshot snapshot;
    auto plan = build_route_plan(false, snapshot, std::nullopt, 4);
    apply_peer_direct_policy(plan, true);
    assert(plan.skip_direct);
    assert(!plan.udp_punch_enabled);
    assert_ms(plan.direct_timeout, 0, "peer-no-direct timeout");
    assert_ms(plan.direct_connect, 0, "peer-no-direct connect timeout");
    assert_reason(plan, "peer_no_direct");
  }

  {
    auto listener = TcpListener::bind(Endpoint{"127.0.0.1", 0});
    RelayPeerInfo peer;
    peer.peer_local_candidates = {"127.0.0.1"};
    peer.peer_listen = Endpoint{"127.0.0.1", listener.local_endpoint().port};
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
    const auto plan = build_route_plan(false, snapshot, stun(StunNatClass::Open), 8);
    assert(!plan.skip_direct);
    assert_ms(plan.direct_timeout, 3500, "open NAT direct window");
    assert_ms(plan.direct_connect, 450, "open NAT connect timeout");
    assert(plan.connections == 8);
    assert_reason(plan, "stun_open");
  }

  {
    ConnectivitySnapshot snapshot;
    const auto plan = build_route_plan(false, snapshot, stun(StunNatClass::Cone), 4);
    assert(!plan.skip_direct);
    assert(!plan.udp_punch_enabled);
    assert_ms(plan.direct_timeout, 2500, "cone NAT direct window");
    assert_reason(plan, "stun_cone_direct_probe");
  }

  {
    ConnectivitySnapshot snapshot;
    const auto plan = build_route_plan(false, snapshot, stun(StunNatClass::Symmetric), 4);
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
    snapshot.self_global_ipv6_count = 1;
    snapshot.peer_global_ipv6_count = 1;
    const auto plan = build_route_plan(false, snapshot, std::nullopt, 4);
    assert(!plan.skip_direct);
    assert_ms(plan.direct_timeout, 3500, "global IPv6 direct window");
    assert_ms(plan.direct_connect, 450, "global IPv6 connect timeout");
    assert_reason(plan, "ipv6_global_direct");
    const auto lan = std::find(plan.direct_candidate_order.begin(), plan.direct_candidate_order.end(), "lan");
    const auto ipv6 = std::find(plan.direct_candidate_order.begin(), plan.direct_candidate_order.end(), "ipv6_global");
    const auto public_v4 = std::find(plan.direct_candidate_order.begin(), plan.direct_candidate_order.end(), "public");
    if (lan == plan.direct_candidate_order.end() || ipv6 == plan.direct_candidate_order.end() ||
        public_v4 == plan.direct_candidate_order.end() || !(lan < ipv6 && ipv6 < public_v4)) {
      std::cerr << "FAIL: global IPv6 route did not keep LAN before IPv6 before public IPv4\n";
      return 1;
    }
  }

  {
    ConnectivitySnapshot snapshot;
    snapshot.self_nat = NatType::BehindNat;
    snapshot.peer_nat = NatType::BehindNat;
    const auto plan = build_route_plan(false, snapshot, std::nullopt, 4);
    assert(!plan.skip_direct);
    assert_ms(plan.direct_timeout, 500, "double NAT direct window");
    assert_ms(plan.direct_connect, 220, "double NAT connect timeout");
    assert_reason(plan, "double_nat_short_punch");
  }

  {
    ConnectivitySnapshot snapshot;
    snapshot.vpn_detected = true;
    snapshot.lan_discovered_count = 2;
    const auto plan = build_route_plan(false, snapshot, std::nullopt, 4);
    assert(!plan.skip_direct);
    assert_ms(plan.direct_timeout, 1000, "VPN LAN direct window");
    assert_ms(plan.direct_connect, 250, "VPN LAN connect timeout");
    assert_reason(plan, "vpn_lan_short_direct");
  }

  {
    ConnectivitySnapshot snapshot;
    snapshot.only_local = true;
    const auto plan = build_route_plan(false, snapshot, std::nullopt, 4);
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
    ConnectivitySnapshot snapshot;
    snapshot.profile.last_path = "direct";
    snapshot.profile.last_direct_candidate_kind = "lan";
    snapshot.profile.candidate_failures_by_kind["public"] = 3;
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
    ConnectivitySnapshot snapshot;
    snapshot.profile.last_path = "relay";
    snapshot.profile.candidate_failures_by_kind["ipv6_global"] = 2;
    snapshot.self_global_ipv6_count = 1;
    snapshot.peer_global_ipv6_count = 1;
    const auto plan = build_route_plan(false, snapshot, std::nullopt, 4);
    assert(!plan.skip_direct);
    assert_ms(plan.direct_timeout, 900, "IPv6 failure profile direct window");
    assert_ms(plan.direct_connect, 250, "IPv6 failure profile connect timeout");
    assert_reason(plan, "profile_ipv6_failures_short_direct");
  }

  {
    ConnectivitySnapshot snapshot;
    snapshot.profile.last_path = "relay";
    snapshot.profile.candidate_failures_by_kind["public"] = 1;
    snapshot.profile.candidate_failures_by_kind["public-same-port"] = 1;
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
    ConnectivitySnapshot snapshot;
    snapshot.profile.last_path = "relay";
    snapshot.profile.path_streak = 2;
    const auto plan = build_route_plan(false, snapshot, std::nullopt, 4);
    assert(!plan.skip_direct);
    assert_ms(plan.direct_timeout, 600, "relay history direct window");
    assert_ms(plan.direct_connect, 220, "relay history connect timeout");
    assert_reason(plan, "profile_relay_history_short_direct");
  }

  {
    ConnectivitySnapshot snapshot;
    snapshot.profile.last_path = "relay";
    snapshot.profile.path_streak = 1;
    const auto plan = build_route_plan(false, snapshot, std::nullopt, 4);
    assert(!plan.skip_direct);
    assert_ms(plan.direct_timeout, 2500, "single relay after direct history should keep default direct window");
    assert_reason(plan, "default");
  }

  {
    ConnectivitySnapshot snapshot;
    snapshot.profile.last_path = "direct";
    snapshot.profile.same_port_attempts = 1;
    snapshot.profile.same_port_successes = 1;
    const auto plan = build_route_plan(false, snapshot, std::nullopt, 4);
    assert_ms(plan.same_port_timeout, 650, "same-port success profile window");
    assert_ms(plan.same_port_connect, 180, "same-port success profile connect window");
  }

  {
    ConnectivitySnapshot snapshot;
    snapshot.profile.last_path = "relay";
    snapshot.profile.same_port_attempts = 4;
    snapshot.profile.same_port_failure_streak = 4;
    const auto plan = build_route_plan(false, snapshot, std::nullopt, 4);
    assert_ms(plan.direct_timeout, 2500, "same-port failures should not shorten whole direct window");
    assert_ms(plan.same_port_timeout, 180, "same-port failure profile window");
    assert_ms(plan.same_port_connect, 100, "same-port failure profile connect window");
    assert_reason(plan, "default");
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
    const auto punch = build_direct_attempt_plan(plan, Role::Receiver, puncher, candidates, open, NatProfile{});
    assert_ms(punch.total_timeout, 600, "planned direct window");
    assert_ms(punch.connect_timeout, 180, "planned connect timeout");
    if (punch.candidates.empty() || punch.candidates.front().kind != "public") {
      std::cerr << "FAIL: planned direct candidate order was not applied\n";
      return 1;
    }
  }

  {
    RoutePlan plan;
    plan.direct_timeout = std::chrono::milliseconds(600);
    plan.direct_connect = std::chrono::milliseconds(180);
    plan.same_port_timeout = std::chrono::milliseconds(120);
    plan.same_port_connect = std::chrono::milliseconds(40);

    AdaptivePuncher puncher;
    const auto punch = build_direct_attempt_plan(plan, Role::Sender, puncher, {}, NatProfile{NatType::Open},
                                                 NatProfile{NatType::BehindNat});
    assert_ms(punch.total_timeout, 600, "route plan clamps adaptive direct window");
    assert_ms(punch.connect_timeout, 180, "route plan overrides adaptive connect timeout");
    assert_ms(punch.same_port_timeout, 120, "route plan overrides same-port window");
    assert_ms(punch.same_port_connect_timeout, 40, "route plan overrides same-port connect timeout");
  }

  {
    RoutePlan plan;
    plan.direct_timeout = std::chrono::milliseconds(5000);
    plan.direct_connect = std::chrono::milliseconds(0);
    plan.same_port_timeout = std::chrono::milliseconds(0);
    plan.same_port_connect = std::chrono::milliseconds(0);

    AdaptivePuncher puncher;
    const auto punch = build_direct_attempt_plan(plan, Role::Sender, puncher, {}, NatProfile{NatType::Open},
                                                 NatProfile{NatType::BehindNat});
    assert_ms(punch.total_timeout, 3500, "route plan does not widen adaptive direct window");
    assert_ms(punch.connect_timeout, 500, "zero route connect timeout preserves adaptive value");
    assert_ms(punch.same_port_timeout, 500, "zero route same-port window preserves adaptive value");
    assert_ms(punch.same_port_connect_timeout, 160,
              "zero route same-port connect timeout preserves adaptive value");
  }

  {
    auto listener = TcpListener::bind(Endpoint{"127.0.0.1", 0});
    RelayPeerInfo peer;
    peer.peer_listen = Endpoint{"127.0.0.1", 1};
    peer.peer_local_candidates = {"2001:db8::5"};
    peer.peer_public = Endpoint{"2001:4860:4860::8888", 5000};
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
          status.find("ipv6_global@[2001:db8::5]:1") != std::string::npos &&
          status.find("reason=global_ipv6_candidate+peer_local_candidates") != std::string::npos &&
          status.find("ipv6_global@[2001:4860:4860::8888]:5000") != std::string::npos &&
          status.find("reason=global_ipv6_candidate+peer_public_host") != std::string::npos) {
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
    RelayPeerInfo peer;
    peer.peer_public = Endpoint{"fd00::1", 5000};
    RoutePlan plan;
    plan.direct_timeout = std::chrono::milliseconds(20);
    plan.direct_connect = std::chrono::milliseconds(5);

    AdaptivePuncher puncher;
    RecordingReporter reporter;
    const auto direct = attempt_direct(Role::Receiver, listener, peer, {}, puncher, NatProfile{}, NatProfile{}, plan,
                                       "ula-public-direct-room", ConnectOptions{}, &reporter);
    assert(!direct);
    bool saw_lan_scoped_ipv6 = false;
    bool mislabeled_public = false;
    for (const auto& status : reporter.statuses) {
      if (status.find("lan@[fd00::1]:5000") != std::string::npos) saw_lan_scoped_ipv6 = true;
      if (status.find("public@[fd00::1]:5000") != std::string::npos) mislabeled_public = true;
    }
    if (!saw_lan_scoped_ipv6 || mislabeled_public) {
      std::cerr << "FAIL: non-global IPv6 relay observation was not scoped as a LAN candidate\n";
      return 1;
    }
  }

  {
    auto listener = TcpListener::bind(Endpoint{"127.0.0.1", 0});
    RelayPeerInfo peer;
    peer.peer_public = Endpoint{"127.0.0.1", 1};
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

  std::cout << "PASS: route scenario decisions\n";
  return 0;
}
