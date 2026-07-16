#include "core/protocol.hpp"
#include "core/progress.hpp"
#include "connect/route_session.hpp"

#include <cassert>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace kiko;

struct RecordingReporter : ProgressReporter {
  std::vector<std::string> statuses;
  std::vector<RoutePhase> phases;
  std::vector<RoutePhaseDetail> phase_details;
  std::vector<RouteOutcome> outcomes;
  std::vector<RouteTiming> timings;

  void status(const std::string& message) override { statuses.push_back(message); }
  void route_phase(RoutePhase phase, const RoutePhaseDetail& detail) override {
    phases.push_back(phase);
    phase_details.push_back(detail);
  }
  void route_outcome(const RouteOutcome& outcome) override { outcomes.push_back(outcome); }
  void route_timing(const RouteTiming& timing) override { timings.push_back(timing); }
};

std::pair<TcpSocket, TcpSocket> connected_pair() {
  auto listener = TcpListener::bind(Endpoint{"127.0.0.1", 0});
  auto client = connect_tcp(listener.local_endpoint(), std::chrono::seconds(2));
  auto server = listener.accept(std::chrono::seconds(2));
  assert(client.valid());
  assert(server.valid());
  return {std::move(server), std::move(client)};
}

bool saw_status(const RecordingReporter& reporter, const std::string& needle) {
  for (const auto& status : reporter.statuses) {
    if (status.find(needle) != std::string::npos) return true;
  }
  return false;
}

bool saw_phase(const RecordingReporter& reporter, RoutePhase phase) {
  for (const auto seen : reporter.phases) {
    if (seen == phase) return true;
  }
  return false;
}

}  // namespace

int main() {
  using namespace kiko;

  {
    auto relay_pair = connected_pair();
    std::atomic_bool cancel{false};
    const auto start = std::chrono::steady_clock::now();
    auto wait = std::async(std::launch::async, [&]() {
      return recv_message_timeout(relay_pair.first, std::chrono::seconds(2), &cancel);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    cancel.store(true);
    auto message = wait.get();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    assert(!message);
    assert(elapsed < std::chrono::milliseconds(500));
  }

  {
    auto relay_pair = connected_pair();
    auto direct_pair = connected_pair();
    std::optional<TcpSocket> direct;
    direct.emplace(std::move(direct_pair.first));

    std::thread peer([relay = std::move(relay_pair.second)]() mutable {
      auto msg = recv_message_timeout(relay, std::chrono::seconds(2));
      assert(msg);
      assert(msg->type == "direct_ok");
      send_message(relay, Message{"direct_start", {}});
    });

    RecordingReporter reporter;
    AdaptivePuncher puncher;
    puncher.observe(PunchObservation{"receiver-active", "127.0.0.1:1234", "lan", 90, true, 7, ""});
    RoutePlan plan;
    auto selection = select_transfer_route(std::move(relay_pair.first), std::move(direct), puncher, plan, reporter,
                                           std::chrono::seconds(2));
    peer.join();

    assert(selection.path == RoutePath::Direct);
    assert(selection.direct);
    assert(saw_status(reporter, "route result: path=direct reason=confirmed direct_attempted=true lan_upgrade=false"));
    assert(saw_status(reporter, "direct probe summary: result=direct candidate_kinds=lan=1"));
    assert(saw_status(
        reporter,
        "route detail: direct_success kind=lan priority=90 endpoint=127.0.0.1:1234 family=ipv4 scope=loopback elapsed_ms=7"));
  }

  {
    auto relay_pair = connected_pair();
    auto direct_pair = connected_pair();
    std::optional<TcpSocket> direct;
    direct.emplace(std::move(direct_pair.first));

    std::thread peer([relay = std::move(relay_pair.second)]() mutable {
      auto msg = recv_message_timeout(relay, std::chrono::seconds(2));
      assert(msg);
      assert(msg->type == "direct_ok");
      send_message(relay, Message{"direct_start", {}});
    });

    RecordingReporter reporter;
    AdaptivePuncher puncher;
    puncher.observe(PunchObservation{"receiver-active", "[2001:4860:4860::8888]:5000", "ipv6_global", 82, true, 11, ""});
    RoutePlan plan;
    auto selection = select_transfer_route(std::move(relay_pair.first), std::move(direct), puncher, plan, reporter,
                                           std::chrono::seconds(2));
    peer.join();

    assert(selection.path == RoutePath::Direct);
    assert(!reporter.outcomes.empty());
    assert(reporter.outcomes.back().direct_candidate_kind == "ipv6_global");
    assert(reporter.outcomes.back().direct_candidate_endpoint == "[2001:4860:4860::8888]:5000");
    assert(reporter.outcomes.back().direct_candidate_family == "ipv6");
    assert(reporter.outcomes.back().direct_candidate_scope == "global");
    assert(saw_status(reporter, "route detail: direct_success kind=ipv6_global priority=82 endpoint=[2001:4860:4860::8888]:5000 family=ipv6 scope=global elapsed_ms=11"));
  }

  {
    auto relay_pair = connected_pair();
    auto direct_pair = connected_pair();
    std::optional<TcpSocket> direct;
    direct.emplace(std::move(direct_pair.first));

    std::thread peer([relay = std::move(relay_pair.second)]() mutable {
      auto standby = recv_message_timeout(relay, std::chrono::seconds(2));
      assert(standby);
      assert(standby->type == "relay_standby");
      auto choice = recv_message_timeout(relay, std::chrono::seconds(2));
      assert(choice);
      assert(choice->type == "direct_ok");
      send_message(relay, Message{"direct_start", {}});
    });

    RecordingReporter reporter;
    AdaptivePuncher puncher;
    puncher.observe(PunchObservation{"receiver-active", "127.0.0.1:1234", "lan", 90, true, 7, ""});
    RoutePlan plan;
    auto selection = race_transfer_route(
        std::move(relay_pair.first),
        [&](const std::atomic_bool*) mutable {
          return std::move(direct);
        },
        puncher, plan, reporter, std::chrono::seconds(2));
    peer.join();

    assert(selection.path == RoutePath::Direct);
    assert(selection.direct);
    assert(saw_status(reporter, "relay standby; trying direct"));
    assert(reporter.phases.size() >= 2);
    assert(reporter.phases[0] == RoutePhase::RelayStandby);
    assert(reporter.phases[1] == RoutePhase::DirectProbing);
    assert(reporter.phase_details[1].relay_fallback_ready);
    assert(selection.timing.direct_probe_ms >= 0);
    assert(!reporter.timings.empty());
    assert(reporter.timings.back().direct_probe_ms >= 0);
    assert(saw_status(reporter, "route result: path=direct reason=confirmed direct_attempted=true lan_upgrade=false"));
  }

  {
    auto relay_pair = connected_pair();
    std::atomic_bool cancel_seen{false};

    std::thread peer([relay = std::move(relay_pair.second)]() mutable {
      auto standby = recv_message_timeout(relay, std::chrono::seconds(2));
      assert(standby);
      assert(standby->type == "relay_standby");
      send_message(relay, Message{"relay_start", {{"reason", "standby"}}});
      auto late = recv_message_timeout(relay, std::chrono::milliseconds(150));
      assert(!late);
    });

    RecordingReporter reporter;
    AdaptivePuncher puncher;
    RoutePlan plan;
    auto selection = race_transfer_route(
        std::move(relay_pair.first),
        [&](const std::atomic_bool* cancel) -> std::optional<TcpSocket> {
          while (!cancel->load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
          cancel_seen.store(true);
          throw std::runtime_error("direct canceled late");
        },
        puncher, plan, reporter, std::chrono::seconds(2));
    peer.join();

    assert(selection.path == RoutePath::Relay);
    assert(selection.allow_lan_upgrade);
    assert(cancel_seen.load());
    assert(saw_phase(reporter, RoutePhase::RelayCommitted));
    assert(saw_status(reporter, "relay committed by peer; canceling direct"));
    assert(saw_status(reporter,
                      "route result: path=relay reason=peer_selected_relay direct_attempted=true lan_upgrade=true"));
    assert(selection.timing.direct_probe_ms >= 0);
    assert(selection.timing.relay_commit_ms >= 0);
    assert(selection.timing.relay_commit_ms < 500);
    assert(!reporter.timings.empty());
    assert(reporter.timings.back().direct_probe_ms >= 0);
    assert(reporter.timings.back().relay_commit_ms >= 0);
  }

  {
    auto relay_pair = connected_pair();
    std::thread peer([relay = std::move(relay_pair.second)]() mutable {
      auto standby = recv_message_timeout(relay, std::chrono::seconds(2));
      assert(standby);
      assert(standby->type == "relay_standby");
      auto choice = recv_message_timeout(relay, std::chrono::seconds(2));
      assert(choice);
      assert(choice->type == "relay_ready");
      send_message(relay, Message{"relay_start", {{"reason", "relay"}}});
    });

    RecordingReporter reporter;
    AdaptivePuncher puncher;
    RoutePlan plan;
    auto selection = race_transfer_route(
        std::move(relay_pair.first),
        [](const std::atomic_bool*) -> std::optional<TcpSocket> {
          throw std::runtime_error("direct worker exploded");
        },
        puncher, plan, reporter, std::chrono::seconds(2));
    peer.join();

    assert(selection.path == RoutePath::Relay);
    assert(selection.allow_lan_upgrade);
    assert(saw_phase(reporter, RoutePhase::RelayCommitted));
    assert(saw_status(reporter, "direct attempt failed: direct worker exploded"));
    assert(saw_status(reporter,
                      "route result: path=relay reason=direct_failed direct_attempted=true lan_upgrade=true"));
    assert(selection.timing.direct_probe_ms >= 0);
    assert(selection.timing.relay_commit_ms >= 0);
    assert(selection.timing.relay_commit_ms < 500);
    assert(!reporter.timings.empty());
    assert(reporter.timings.back().direct_probe_ms >= 0);
    assert(reporter.timings.back().relay_commit_ms >= 0);
  }

  {
    auto relay_pair = connected_pair();
    std::thread peer([relay = std::move(relay_pair.second)]() mutable {
      auto standby = recv_message_timeout(relay, std::chrono::seconds(2));
      assert(standby);
      assert(standby->type == "relay_standby");
      auto choice = recv_message_timeout(relay, std::chrono::seconds(2));
      assert(choice);
      assert(choice->type == "relay_ready");
      send_message(relay, Message{"relay_start", {{"reason", "relay"}}});
    });

    RecordingReporter reporter;
    AdaptivePuncher puncher;
    puncher.observe(PunchObservation{"receiver-active", "192.168.1.8:5000", "lan", 90, false, 100,
                                     "connect_failed"});
    RoutePlan plan;
    auto selection = race_transfer_route(
        std::move(relay_pair.first),
        [](const std::atomic_bool*) -> std::optional<TcpSocket> {
          return std::nullopt;
        },
        puncher, plan, reporter, std::chrono::seconds(2));
    peer.join();

    assert(selection.path == RoutePath::Relay);
    assert(selection.allow_lan_upgrade);
    assert(saw_status(reporter,
                      "route result: path=relay reason=direct_failed direct_attempted=true lan_upgrade=true"));
    assert(selection.timing.direct_probe_ms >= 0);
    assert(selection.timing.relay_commit_ms >= 0);
    assert(selection.timing.relay_commit_ms < 500);
  }

  {
    auto relay_pair = connected_pair();
    std::thread peer([relay = std::move(relay_pair.second)]() mutable {
      auto msg = recv_message_timeout(relay, std::chrono::seconds(2));
      assert(msg);
      assert(msg->type == "relay_ready");
      send_message(relay, Message{"relay_start", {}});
    });

    RecordingReporter reporter;
    AdaptivePuncher puncher;
    puncher.observe(PunchObservation{"sync-same-port", "203.0.113.10:5000", "public-same-port", 20, false, 80,
                                     "connect_failed"});
    puncher.observe(PunchObservation{"receiver-active", "192.168.1.8:5000", "lan", 90, false, 100,
                                     "direct_ack_failed"});
    RoutePlan plan;
    auto selection = select_transfer_route(std::move(relay_pair.first), std::nullopt, puncher, plan, reporter,
                                           std::chrono::seconds(2));
    peer.join();

    assert(selection.path == RoutePath::Relay);
    assert(selection.allow_lan_upgrade);
    assert(saw_status(reporter,
                      "route result: path=relay reason=direct_failed direct_attempted=true lan_upgrade=true"));
    assert(saw_status(
        reporter,
        "direct probe summary: result=relay_fallback candidate_kinds=lan=1,public-same-port=1 same_port=attempts=1,successes=0,failures=1,last_ms=80"));
    assert(saw_status(reporter, "route detail: direct_failed"));
    assert(saw_status(reporter, "failures=connect_failed=1,direct_ack_failed=1"));
    assert(saw_status(reporter, "candidate_kinds=lan=1,public-same-port=1"));
  }

  {
    auto relay_pair = connected_pair();
    std::thread peer([relay = std::move(relay_pair.second)]() mutable {
      auto msg = recv_message_timeout(relay, std::chrono::seconds(2));
      assert(msg);
      assert(msg->type == "relay_ready");
      send_message(relay, Message{"relay_start", {{"reason", "mismatch"}}});
    });

    RecordingReporter reporter;
    AdaptivePuncher puncher;
    RoutePlan plan;
    plan.skip_direct = true;
    auto selection = select_transfer_route(std::move(relay_pair.first), std::nullopt, puncher, plan, reporter,
                                           std::chrono::seconds(2));
    peer.join();

    assert(selection.path == RoutePath::Relay);
    assert(!selection.allow_lan_upgrade);
    assert(saw_status(reporter,
                      "route result: path=relay reason=direct_skipped direct_attempted=false lan_upgrade=false"));
    assert(saw_status(reporter, "route detail: direct_not_attempted"));
  }

  std::cout << "PASS: route session reports final route decisions\n";
  return 0;
}
