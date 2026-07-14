#include "web/web_job.hpp"

#include <cassert>
#include <chrono>
#include <iostream>

using namespace kiko;

int main() {
  WebJobStore store;
  WebReporter reporter(store);

  auto snapshot = store.snapshot();
  assert(snapshot.kind.empty());
  assert(!snapshot.running);
  assert(snapshot.activity.empty());
  assert(snapshot.started == std::chrono::steady_clock::time_point{});

  reporter.code_ready("test-code", false);
  reporter.route_phase(RoutePhase::DirectProbing, RoutePhaseDetail{"probing peers", {}, true});
  reporter.route_outcome(RouteOutcome{"relay", "direct", "connected"});
  reporter.route_timing(RouteTiming{12, 34, -1, 5});
  reporter.transfer_overview(2, 300);
  reporter.file_start("a.txt", 100);
  reporter.file_advance(40);
  reporter.file_complete("a.txt", 100, true);

  snapshot = store.snapshot();
  assert(snapshot.code == "test-code");
  assert(snapshot.route_phase == "direct connect (relay ready)");
  assert(snapshot.route_summary.find("data=direct") != std::string::npos);
  assert(snapshot.route_timing == "rendezvous=12ms direct_probe=34ms securing=5ms");
  assert(snapshot.files_total == 2);
  assert(snapshot.files_done == 1);
  assert(snapshot.overall_total == 300);
  assert(snapshot.overall_done == 40);
  assert(snapshot.current_file == "a.txt");

  reporter.handshake_ok();
  snapshot = store.snapshot();
  assert(snapshot.handshake);
  assert(snapshot.activity == "encrypted channel ready");

  reporter.transfer_complete(2, 300);
  snapshot = store.snapshot();
  assert(snapshot.finished);
  assert(snapshot.files_done == 2);
  assert(snapshot.overall_done == 300);

  std::string error;
  assert(!store.update_note("not running", error));
  assert(error == "notepad is not running");

  error.clear();
  assert(!store.start_send(SendConfig{}, error));
  assert(error == "send path is required");

  error.clear();
  assert(!store.start_recv(RecvConfig{}, error));
  assert(error == "receive code is required");

  NoteConfig note;
  note.role = Role::Receiver;
  error.clear();
  assert(!store.start_note(note, error));
  assert(error == "note code is required");

  for (int i = 0; i < 130; ++i) reporter.status("line " + std::to_string(i));
  snapshot = store.snapshot();
  assert(snapshot.logs.size() == 120);
  assert(snapshot.logs.front() == "line 10");
  assert(snapshot.logs.back() == "line 129");

  std::cout << "web job ok\n";
  return 0;
}
