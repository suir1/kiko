#include "core/progress_state.hpp"

#include <cassert>
#include <chrono>
#include <iostream>

using namespace kiko;

int main() {
  TransferProgressState state(3);

  state.route_phase_changed(RoutePhase::DirectProbing, RoutePhaseDetail{"trying direct", "default", true});
  assert(state.route_phase == "direct connect (relay ready)");
  assert(state.activity == "trying direct");
  assert(state.logs.back() == "route phase: direct connect (relay ready) (default) relay-ready");

  RouteOutcome outcome;
  outcome.data_path = "direct";
  outcome.reason = "connected";
  outcome.direct_attempted = true;
  outcome.direct_candidate_kind = "lan";
  outcome.direct_candidate_endpoint = "192.0.2.10:9000";
  outcome.direct_candidate_family = "ipv4";
  outcome.direct_candidate_scope = "private";
  outcome.direct_elapsed_ms = 24;
  state.route_selected(outcome);
  assert(state.route_summary ==
         "control=relay data=direct (connected) via lan 192.0.2.10:9000 ipv4/private 24ms");
  assert(state.route_phase == "direct TCP selected");

  state.transfer_overview_received(2, 300);
  state.file_started("a.txt", 100);
  assert(state.file_advanced(40));
  state.file_completed();
  assert(state.current_done == 40);
  assert(state.overall_done == 40);
  assert(state.files_done == 1);

  state.transfer_retrying(2, 3, "connection reset");
  state.transfer_retry_waiting(2, 3, std::chrono::milliseconds(250));
  assert(state.current_file.empty());
  assert(state.current_done == 0);
  assert(state.overall_done == 0);
  assert(state.files_done == 0);
  assert(state.route_phase == "reconnecting");
  assert(state.activity == "waiting to reconnect 2/3");
  assert(state.logs.size() == 3);

  state.transfer_completed(2, 300);
  assert(state.finished);
  assert(state.overall_done == 300);
  assert(!state.file_advanced(1));

  state.reset();
  assert(!state.finished);
  assert(state.logs.empty());
  assert(state.activity == "starting...");

  state.append_log("one\ntwo\nthree\nfour");
  assert(state.logs.size() == 3);
  assert(state.logs.front() == "two");
  assert(state.joined_logs() == "two\nthree\nfour");

  state.finish_failed("boom");
  assert(state.finished);
  assert(state.failed);
  assert(!state.canceled);
  assert(state.error == "boom");
  assert(state.logs.back() == "error: boom");

  state.finish_canceled();
  assert(state.finished);
  assert(!state.failed);
  assert(state.canceled);
  assert(state.error.empty());

  std::cout << "progress state ok\n";
  return 0;
}
