#include "connect/connectivity.hpp"
#include "connect/direct_session.hpp"
#include "connect/route_planner.hpp"
#include "relay/relay_race.hpp"
#include "transfer/transfer.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <thread>

int main() {
  using namespace kiko;

  {
    ConnectivitySnapshot snap;
    auto plan = build_route_plan(true, snap, std::nullopt, 4);
    assert(plan.skip_direct);
    assert(plan.reason == "no_direct");
  }

  {
    assert(!should_run_stun_probe(false, false, false));
    assert(should_run_stun_probe(true, false, false));
    assert(should_run_stun_probe(false, true, false));
    assert(should_run_stun_probe(false, false, true));
  }

  {
    StunProbeResult stun;
    stun.ok = true;
    stun.nat_class = StunNatClass::Symmetric;
    stun.mapped = Endpoint{"203.0.113.1", 54321};
    ConnectivitySnapshot snap;
    auto plan = build_route_plan(false, snap, stun, 4);
    assert(!plan.skip_direct);
    assert(plan.direct_timeout.count() == 500);
    assert(plan.direct_connect.count() == 220);
    assert(plan.reason == "stun_symmetric_short_direct");
  }

  {
    ConnectivitySnapshot snap;
    snap.vpn_detected = true;
    snap.lan_discovered_count = 2;
    auto plan = build_route_plan(false, snap, std::nullopt, 4);
    assert(!plan.skip_direct);
    assert(plan.direct_timeout.count() == 1000);
    assert(plan.direct_connect.count() == 250);
    assert(plan.reason == "vpn_lan_short_direct");
  }

  {
    ConnectivitySnapshot snap;
    snap.self_nat = NatType::BehindNat;
    snap.peer_nat = NatType::BehindNat;
    snap.self_global_ipv6_count = 1;
    snap.peer_global_ipv6_count = 1;
    auto plan = build_route_plan(false, snap, std::nullopt, 4);
    assert(!plan.skip_direct);
    assert(plan.direct_timeout.count() == 3500);
    assert(plan.direct_connect.count() == 450);
    assert(plan.reason == "ipv6_global_direct");
    const auto lan = std::find(plan.direct_candidate_order.begin(), plan.direct_candidate_order.end(), "lan");
    const auto ipv6 = std::find(plan.direct_candidate_order.begin(), plan.direct_candidate_order.end(), "ipv6_global");
    const auto public_v4 = std::find(plan.direct_candidate_order.begin(), plan.direct_candidate_order.end(), "public");
    assert(lan != plan.direct_candidate_order.end());
    assert(ipv6 != plan.direct_candidate_order.end());
    assert(public_v4 != plan.direct_candidate_order.end());
    assert(lan < ipv6);
    assert(ipv6 < public_v4);
  }

  {
    ConnectivitySnapshot snap;
    snap.self_nat = NatType::BehindNat;
    snap.peer_nat = NatType::BehindNat;
    auto plan = build_route_plan(false, snap, std::nullopt, 4);
    assert(!plan.skip_direct);
    assert(plan.direct_timeout.count() == 500);
    assert(plan.direct_connect.count() == 220);
    assert(plan.reason == "double_nat_short_punch");
  }

  {
    StunProbeResult stun;
    stun.ok = true;
    stun.nat_class = StunNatClass::Open;
    stun.mapped = Endpoint{"192.168.1.5", 40000};
    ConnectivitySnapshot snap;
    auto plan = build_route_plan(false, snap, stun, 8);
    assert(!plan.skip_direct);
    assert(plan.direct_timeout.count() == 3500);
    assert(plan.connections == 8);
    assert(plan.reason == "stun_open");
  }

  {
    StunProbeResult stun;
    stun.ok = true;
    stun.nat_class = StunNatClass::Cone;
    stun.mapped = Endpoint{"203.0.113.2", 40000};
    ConnectivitySnapshot snap;
    auto plan = build_route_plan(false, snap, stun, 4);
    assert(!plan.skip_direct);
    assert(!plan.udp_punch_enabled);
    assert(plan.reason == "stun_cone_direct_probe");
  }

  {
    StunProbeResult stun;
    stun.ok = true;
    stun.nat_class = StunNatClass::Cone;
    ConnectivitySnapshot snap;
    auto plan = build_route_plan(true, snap, stun, 4);
    assert(plan.skip_direct);
    assert(!plan.udp_punch_enabled);
  }

  {
    ConnectivitySnapshot snap;
    snap.stun_nat = StunNatClass::Cone;
    auto plan = build_route_plan(false, snap, std::nullopt, 4);
    assert(!plan.udp_punch_enabled);
    assert(plan.reason == "stun_cone_direct_probe");
  }

  {
    Endpoint external{"127.0.0.1", 9000};
    std::vector<RelayRaceEntry> entries{{Endpoint{"127.0.0.1", 12345}, false}, {external, true}};
    apply_relay_kind_order(entries, {"external", "embedded"}, external);
    assert(entries.front().endpoint.host == external.host);
    assert(entries.front().endpoint.port == external.port);
  }

  {
    std::vector<DirectCandidate> candidates{
        make_direct_candidate(Endpoint{"203.0.113.7", 5000}, "public", 20),
        make_direct_candidate(Endpoint{"192.168.1.10", 5000}, "lan", 90),
        make_direct_candidate(Endpoint{"198.51.100.9", 5000}, "listen", 60),
    };
    apply_direct_candidate_scoring(candidates, RoutePlan::DirectCandidateScoreHints{}, {"public", "listen"});
    AdaptivePuncher puncher;
    auto plan = puncher.plan(Role::Receiver, candidates);
    if (plan.candidates.front().kind != "public") {
      std::cerr << "FAIL: AI direct candidate order did not boost public first\n";
      return 1;
    }
  }

  {
    std::vector<DirectCandidate> candidates{
        make_direct_candidate(Endpoint{"203.0.113.7", 5000}, "public", 20),
        make_direct_candidate(Endpoint{"192.168.1.10", 5000}, "lan", 90),
        make_direct_candidate(Endpoint{"2001:4860:4860::8888", 5000}, "ipv6_global", 82),
    };
    RoutePlan::DirectCandidateScoreHints hints;
    hints.vpn_detected = true;
    hints.profile_last_path = "direct";
    hints.profile_direct_candidate_kind = "lan";
    hints.profile_candidate_failures_by_kind["public"] = 4;
    apply_direct_candidate_scoring(candidates, hints, {"public"});

    auto has_reason = [](const DirectCandidate& candidate, const std::string& reason) {
      return std::find(candidate.reasons.begin(), candidate.reasons.end(), reason) != candidate.reasons.end();
    };
    assert(candidates[0].priority == 1000);
    assert(candidates[1].priority == 105);
    assert(candidates[2].priority == 82);
    assert(has_reason(candidates[0], "route_order_hint"));
    assert(has_reason(candidates[0], "profile_previous_failure"));
    assert(has_reason(candidates[1], "profile_direct_success"));
    assert(has_reason(candidates[1], "vpn_lan_caution"));
  }

  {
    auto sender_listener = TcpListener::bind(Endpoint{"127.0.0.1", 0});
    auto receiver_listener = TcpListener::bind(Endpoint{"127.0.0.1", 0});

    PunchPlan sender_plan;
    sender_plan.candidates = {make_direct_candidate(receiver_listener.local_endpoint(), "listen", 60)};
    sender_plan.total_timeout = std::chrono::milliseconds(1200);
    sender_plan.connect_timeout = std::chrono::milliseconds(120);
    sender_plan.retry_delay = std::chrono::milliseconds(20);

    PunchPlan receiver_plan = sender_plan;
    receiver_plan.candidates = {make_direct_candidate(sender_listener.local_endpoint(), "lan", 90)};

    AdaptivePuncher sender_puncher;
    AdaptivePuncher receiver_puncher;
    auto sender_future = std::async(std::launch::async, [&] {
      return try_direct_with_plan(Role::Sender, sender_listener, sender_plan, sender_puncher, "direct-room-1");
    });
    auto receiver_future = std::async(std::launch::async, [&] {
      return try_direct_with_plan(Role::Receiver, receiver_listener, receiver_plan, receiver_puncher, "direct-room-1");
    });

    auto sender_socket = sender_future.get();
    auto receiver_socket = receiver_future.get();
    if (!sender_socket || !receiver_socket || !sender_socket->valid() || !receiver_socket->valid()) {
      std::cerr << "FAIL: receiver-active direct race did not connect\n";
      return 1;
    }
    send_message(*sender_socket, Message{"probe", {{"ok", "1"}}});
    auto probe = recv_message_timeout(*receiver_socket, std::chrono::milliseconds(500));
    if (!probe || probe->type != "probe" || probe->get("ok") != "1") {
      std::cerr << "FAIL: receiver-active direct sockets were not paired\n";
      return 1;
    }
    const auto sender_report = sender_puncher.report();
    const auto receiver_report = receiver_puncher.report();
    if (sender_report.find("phase=receiver-active-accept") == std::string::npos ||
        receiver_report.find("phase=receiver-active") == std::string::npos ||
        receiver_report.find("kind=lan") == std::string::npos) {
      std::cerr << "FAIL: direct punch report did not include phase and candidate details\n";
      return 1;
    }
  }

  {
    auto sender_listener = TcpListener::bind(Endpoint{"127.0.0.1", 0});
    auto receiver_listener = TcpListener::bind(Endpoint{"127.0.0.1", 0});

    PunchPlan sender_plan;
    sender_plan.candidates = {make_direct_candidate(receiver_listener.local_endpoint(), "listen", 60)};
    sender_plan.total_timeout = std::chrono::milliseconds(1000);
    sender_plan.connect_timeout = std::chrono::milliseconds(80);
    sender_plan.retry_delay = std::chrono::milliseconds(20);

    PunchPlan receiver_plan = sender_plan;
    receiver_plan.candidates = {make_direct_candidate(Endpoint{"127.0.0.1", 1}, "public", 20)};

    AdaptivePuncher sender_puncher;
    AdaptivePuncher receiver_puncher;
    auto sender_future = std::async(std::launch::async, [&] {
      return try_direct_with_plan(Role::Sender, sender_listener, sender_plan, sender_puncher, "direct-room-2");
    });
    auto receiver_future = std::async(std::launch::async, [&] {
      return try_direct_with_plan(Role::Receiver, receiver_listener, receiver_plan, receiver_puncher, "direct-room-2");
    });

    auto sender_socket = sender_future.get();
    auto receiver_socket = receiver_future.get();
    if (!sender_socket || !receiver_socket || !sender_socket->valid() || !receiver_socket->valid()) {
      std::cerr << "FAIL: sender-active direct race did not connect\n";
      return 1;
    }
    send_message(*receiver_socket, Message{"probe", {{"ok", "1"}}});
    auto probe = recv_message_timeout(*sender_socket, std::chrono::milliseconds(500));
    if (!probe || probe->type != "probe" || probe->get("ok") != "1") {
      std::cerr << "FAIL: sender-active direct sockets were not paired\n";
      return 1;
    }
    const auto sender_report = sender_puncher.report();
    if (sender_report.find("phase=sender-active") == std::string::npos ||
        sender_report.find("kind=listen") == std::string::npos) {
      std::cerr << "FAIL: sender-active punch report did not include phase and candidate details\n";
      return 1;
    }
  }

  {
    auto listener = TcpListener::bind(Endpoint{"127.0.0.1", 0});
    PunchPlan plan;
    plan.total_timeout = std::chrono::seconds(3);
    plan.connect_timeout = std::chrono::milliseconds(100);
    plan.retry_delay = std::chrono::milliseconds(20);

    AdaptivePuncher puncher;
    std::atomic_bool cancel{false};
    std::atomic_bool peer_connected{false};
    auto direct_future = std::async(std::launch::async, [&] {
      return try_direct_with_plan(Role::Sender, listener, plan, puncher, "cancel-accept-preflight",
                                  ConnectOptions{}, "", &cancel);
    });

    std::thread silent_peer([&] {
      auto peer = connect_tcp(listener.local_endpoint(), std::chrono::seconds(2));
      peer_connected.store(peer.valid());
      while (peer.valid() && !cancel.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    });

    const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!peer_connected.load() && std::chrono::steady_clock::now() < wait_deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!peer_connected.load()) {
      cancel.store(true);
      silent_peer.join();
      std::cerr << "FAIL: silent direct peer did not connect\n";
      return 1;
    }

    const auto cancel_start = std::chrono::steady_clock::now();
    cancel.store(true);
    auto direct = direct_future.get();
    const auto elapsed = std::chrono::steady_clock::now() - cancel_start;
    silent_peer.join();
    if (direct) {
      std::cerr << "FAIL: canceled direct preflight unexpectedly succeeded\n";
      return 1;
    }
    if (elapsed >= std::chrono::milliseconds(500)) {
      std::cerr << "FAIL: direct accept preflight ignored cancellation\n";
      return 1;
    }
  }

  {
    auto listener = TcpListener::bind(Endpoint{"127.0.0.1", 0});
    const auto endpoint = listener.local_endpoint();
    std::thread silent_relay([&] {
      auto accepted = listener.accept(std::chrono::seconds(2));
      if (accepted.valid()) std::this_thread::sleep_for(std::chrono::seconds(1));
    });

    const auto start = std::chrono::steady_clock::now();
    const auto rtt = probe_relay_rtt_ms(endpoint);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    silent_relay.join();
    if (rtt != -1) {
      std::cerr << "FAIL: silent relay probe reported success\n";
      return 1;
    }
    if (elapsed >= std::chrono::milliseconds(900)) {
      std::cerr << "FAIL: silent relay probe blocked too long\n";
      return 1;
    }
  }

  {
    auto listener = TcpListener::bind(Endpoint{"127.0.0.1", 0});
    const auto endpoint = listener.local_endpoint();
    std::thread silent_relay([&] {
      auto accepted = listener.accept(std::chrono::seconds(2));
      if (accepted.valid()) std::this_thread::sleep_for(std::chrono::seconds(1));
    });

    const std::vector<RelayRaceEntry> entries{{endpoint, false}};
    RelayHello hello;
    hello.room = "silent-race";
    const auto start = std::chrono::steady_clock::now();
    auto result = race_until_peer(entries, hello, std::chrono::milliseconds(200), ConnectOptions{});
    const auto elapsed = std::chrono::steady_clock::now() - start;
    silent_relay.join();
    if (result) {
      std::cerr << "FAIL: silent relay race unexpectedly produced a peer\n";
      return 1;
    }
    if (elapsed >= std::chrono::milliseconds(900)) {
      std::cerr << "FAIL: silent relay race ignored deadline\n";
      return 1;
    }
  }

  {
    auto sender_listener = TcpListener::bind(Endpoint{"127.0.0.1", 0});
    auto receiver_listener = TcpListener::bind(Endpoint{"127.0.0.1", 0});
    TcpSocket sender_primary;
    TcpSocket receiver_primary;
    std::thread connector([&] {
      receiver_primary = connect_tcp(sender_listener.local_endpoint(), std::chrono::seconds(2));
    });
    sender_primary = sender_listener.accept(std::chrono::seconds(2));
    connector.join();
    if (!sender_primary.valid() || !receiver_primary.valid()) {
      std::cerr << "FAIL: could not establish primary direct test channel\n";
      return 1;
    }

    RelayPeerInfo sender_peer;
    sender_peer.peer_listen = Endpoint{"127.0.0.1", receiver_listener.local_endpoint().port};
    RelayPeerInfo receiver_peer;
    receiver_peer.peer_listen = Endpoint{"127.0.0.1", 1};
    constexpr auto setup_timeout = std::chrono::milliseconds(200);
    auto sender_future =
        std::async(std::launch::async,
                   [primary = std::move(sender_primary), &sender_listener, sender_peer, setup_timeout]() mutable {
	          return negotiate_direct_mux_channels(std::move(primary), Role::Sender, sender_listener, sender_peer, 2,
	                                               "direct-mux-fallback", ConnectOptions{}, setup_timeout);
        });
    auto receiver_future =
        std::async(std::launch::async,
                   [primary = std::move(receiver_primary), &receiver_listener, receiver_peer, setup_timeout]() mutable {
	          return negotiate_direct_mux_channels(std::move(primary), Role::Receiver, receiver_listener, receiver_peer, 2,
	                                               "direct-mux-fallback", ConnectOptions{}, setup_timeout);
        });
    auto sender_mux = sender_future.get();
    auto receiver_mux = receiver_future.get();
    if (sender_mux.mux_enabled || receiver_mux.mux_enabled || sender_mux.channels.size() != 1 ||
        receiver_mux.channels.size() != 1) {
      std::cerr << "FAIL: direct mux did not downgrade to single channel after auxiliary failure\n";
      return 1;
    }
    send_message(sender_mux.channels[0], Message{"probe", {{"ok", "1"}}});
    auto probe = recv_message_timeout(receiver_mux.channels[0], std::chrono::milliseconds(500));
    if (!probe || probe->type != "probe" || probe->get("ok") != "1") {
      std::cerr << "FAIL: primary direct channel was not usable after mux downgrade\n";
      return 1;
    }
  }

  std::cout << "connectivity_test ok\n";
  return 0;
}
