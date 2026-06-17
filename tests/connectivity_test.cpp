#include "connectivity.hpp"
#include "direct_session.hpp"
#include "relay_race.hpp"
#include "transfer.hpp"

#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <thread>

int main() {
  using namespace kiko;

  RuleScheduler scheduler;

  {
    ConnectivitySnapshot snap;
    auto plan = scheduler.plan(snap, std::nullopt, true, 4);
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
    auto plan = scheduler.plan(snap, stun, false, 4);
    assert(!plan.skip_direct);
    assert(plan.direct_timeout.count() == 700);
    assert(plan.direct_connect.count() == 220);
    assert(plan.reason == "stun_symmetric_short_direct");
  }

  {
    ConnectivitySnapshot snap;
    snap.vpn_detected = true;
    snap.lan_discovered_count = 2;
    auto plan = scheduler.plan(snap, std::nullopt, false, 4);
    assert(!plan.skip_direct);
    assert(plan.direct_timeout.count() == 1000);
    assert(plan.direct_connect.count() == 250);
    assert(plan.reason == "vpn_lan_short_direct");
  }

  {
    ConnectivitySnapshot snap;
    snap.self_nat = NatType::BehindNat;
    snap.peer_nat = NatType::BehindNat;
    auto plan = scheduler.plan(snap, std::nullopt, false, 4);
    assert(!plan.skip_direct);
    assert(plan.direct_timeout.count() == 800);
    assert(plan.reason == "double_nat_short_punch");
  }

  {
    StunProbeResult stun;
    stun.ok = true;
    stun.nat_class = StunNatClass::Open;
    stun.mapped = Endpoint{"192.168.1.5", 40000};
    ConnectivitySnapshot snap;
    auto plan = scheduler.plan(snap, stun, false, 8);
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
    auto plan = scheduler.plan(snap, stun, false, 4);
    assert(!plan.skip_direct);
    assert(!plan.udp_punch_enabled);
    assert(plan.reason == "stun_cone_direct_probe");
  }

  {
    StunProbeResult stun;
    stun.ok = true;
    stun.nat_class = StunNatClass::Cone;
    ConnectivitySnapshot snap;
    auto plan = scheduler.plan(snap, stun, true, 4);
    assert(plan.skip_direct);
    assert(!plan.udp_punch_enabled);
  }

  {
    ConnectivitySnapshot snap;
    snap.stun_nat = StunNatClass::Cone;
    auto plan = scheduler.plan(snap, std::nullopt, false, 4);
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
    apply_direct_candidate_kind_order(candidates, {"public", "listen"});
    AdaptivePuncher puncher;
    auto plan = puncher.plan(Role::Receiver, candidates);
    if (plan.candidates.front().kind != "public") {
      std::cerr << "FAIL: AI direct candidate order did not boost public first\n";
      return 1;
    }
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
    Message hello{"hello", {{"room", "silent-race"}, {"role", "sender"}}};
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

    Message sender_peer{"peer",
                        {{"peer_listen_host", "127.0.0.1"},
                         {"peer_listen_port", std::to_string(receiver_listener.local_endpoint().port)}}};
    Message receiver_peer{"peer", {{"peer_listen_host", "127.0.0.1"}, {"peer_listen_port", "1"}}};
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
