#include "platform.hpp"
#include "protocol.hpp"
#include "progress.hpp"
#include "route_session.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace kiko;

struct RecordingReporter : ProgressReporter {
  std::vector<std::string> statuses;

  void status(const std::string& message) override { statuses.push_back(message); }
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

}  // namespace

int main() {
  using namespace kiko;

  {
    auto relay_pair = connected_pair();
    auto direct_pair = connected_pair();
    std::optional<TcpSocket> direct;
    direct.emplace(std::move(direct_pair.first));

    std::thread peer([relay = std::move(relay_pair.second)]() mutable {
      auto msg = recv_message(relay);
      assert(msg);
      assert(msg->type == "direct_ok");
      send_message(relay, Message{"direct_start", {}});
    });

    RecordingReporter reporter;
    AdaptivePuncher puncher;
    RoutePlan plan;
    auto selection = select_transfer_route(std::move(relay_pair.first), std::move(direct), puncher, plan, reporter,
                                           std::chrono::seconds(2));
    peer.join();

    assert(selection.path == RoutePath::Direct);
    assert(selection.direct);
    assert(saw_status(reporter, "route result: path=direct reason=confirmed direct_attempted=true lan_upgrade=false"));
  }

  {
    auto relay_pair = connected_pair();
    std::thread peer([relay = std::move(relay_pair.second)]() mutable {
      auto msg = recv_message(relay);
      assert(msg);
      assert(msg->type == "relay_ready");
      send_message(relay, Message{"relay_start", {}});
    });

    RecordingReporter reporter;
    AdaptivePuncher puncher;
    RoutePlan plan;
    auto selection = select_transfer_route(std::move(relay_pair.first), std::nullopt, puncher, plan, reporter,
                                           std::chrono::seconds(2));
    peer.join();

    assert(selection.path == RoutePath::Relay);
    assert(selection.allow_lan_upgrade);
    assert(saw_status(reporter,
                      "route result: path=relay reason=direct_failed direct_attempted=true lan_upgrade=true"));
  }

  {
    auto relay_pair = connected_pair();
    std::thread peer([relay = std::move(relay_pair.second)]() mutable {
      auto msg = recv_message(relay);
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
  }

  std::cout << "PASS: route session reports final route decisions\n";
  return 0;
}
