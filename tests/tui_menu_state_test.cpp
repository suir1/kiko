#include "tui/tui_menu_state.hpp"
#include "tui/tui_transfer_view.hpp"

#include "platform/platform.hpp"

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;

namespace {

bool contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

std::string render_transfer_text(const kiko::TuiState& state) {
  auto document = kiko::render_transfer_view(state);
  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(100), ftxui::Dimension::Fixed(40));
  ftxui::Render(screen, document);
  return screen.ToString();
}

std::string line_containing(const std::string& text, const std::string& needle) {
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    if (contains(line, needle)) return line;
  }
  return {};
}

int expect_error(kiko::TuiMenuState state, const std::string& expected) {
  const auto prepared = kiko::prepare_tui_transfer(state);
  if (prepared.ok || !contains(prepared.error, expected)) {
    std::cerr << "FAIL: expected error containing '" << expected << "', got '"
              << (prepared.ok ? std::string("<ok>") : prepared.error) << "'\n";
    return 1;
  }
  return 0;
}

kiko::TuiMenuState base_menu() {
  kiko::TuiMenuState state;
  state.relay = "127.0.0.1:9000";
  state.connections_text = "4";
  return state;
}

}  // namespace

int main() {
  using namespace kiko;

  {
    auto state = base_menu();
    state.mode = 0;
    if (expect_error(state, "path is required") != 0) return 1;
  }

  {
    auto state = base_menu();
    state.mode = 1;
    if (expect_error(state, "pairing code is required") != 0) return 1;
  }

  {
    auto state = base_menu();
    state.mode = 1;
    state.code = "abc234";
    state.relay = "127.0.0.1:0";
    if (expect_error(state, "invalid relay") != 0) return 1;
  }

  {
    auto state = base_menu();
    state.mode = 1;
    state.code = "abc234";
    state.connections_text = "many";
    if (expect_error(state, "connections must be a number") != 0) return 1;
  }

  const auto send_path = fs::temp_directory_path() / ("kiko_tui_menu_state_test_" + std::to_string(process_id()));
  {
    std::ofstream out(send_path);
    out << "hello kiko\n";
  }

  {
    auto state = base_menu();
    state.mode = 0;
    state.path = send_path.string();
    state.connections_text = "8";
    state.network.no_direct = true;

    const auto prepared = prepare_tui_transfer(state);
    if (!prepared.ok || prepared.title != "kiko send" || prepared.spec.mode != 0 ||
        prepared.spec.path != send_path.string() || prepared.spec.relay.host != "127.0.0.1" ||
        prepared.spec.relay.port != 9000 || prepared.spec.network.connections != 8 ||
        !prepared.spec.network.no_direct) {
      std::cerr << "FAIL: send transfer spec was not prepared correctly\n";
      fs::remove(send_path);
      return 1;
    }
  }

  {
    auto state = base_menu();
    state.mode = 2;
    state.note_role = 0;
    state.network.no_direct = true;
    const auto prepared = prepare_tui_note(state);
    if (!prepared.ok || prepared.config.role != Role::Sender || !prepared.config.code.empty() ||
        prepared.config.relay.host != "127.0.0.1" || !prepared.config.no_direct ||
        prepared.config.app != "note") {
      std::cerr << "FAIL: notepad host config was not prepared correctly\n";
      fs::remove(send_path);
      return 1;
    }
  }

  {
    auto state = base_menu();
    state.mode = 2;
    state.note_role = 1;
    if (prepare_tui_note(state).ok) {
      std::cerr << "FAIL: notepad join should require a pairing code\n";
      fs::remove(send_path);
      return 1;
    }
    state.code = "abc234";
    const auto prepared = prepare_tui_note(state);
    if (!prepared.ok || prepared.config.role != Role::Receiver || prepared.config.code != "abc234") {
      std::cerr << "FAIL: notepad join config was not prepared correctly\n";
      fs::remove(send_path);
      return 1;
    }
  }

  {
    auto state = base_menu();
    state.mode = 1;
    state.code = " R561 \n";
    const auto prepared = prepare_tui_transfer(state);
    if (!prepared.ok || prepared.spec.code != "r561") {
      std::cerr << "FAIL: receive should normalize alphanumeric pairing code R561\n";
      fs::remove(send_path);
      return 1;
    }
  }

  {
    auto state = base_menu();
    state.mode = 2;
    state.note_role = 1;
    state.code = " 4827-Stone-IRIS ";
    const auto prepared = prepare_tui_note(state);
    if (!prepared.ok || prepared.config.code != "4827-stone-iris") {
      std::cerr << "FAIL: notepad join should normalize mnemonic pairing code\n";
      fs::remove(send_path);
      return 1;
    }
  }

  {
    auto state = base_menu();
    state.mode = 1;
    state.code = "abc234";
    state.output_dir = "/tmp/kiko-out";
    state.relay = "relay.example:9000";
    state.relay_pass = "secret";

    const auto prepared = prepare_tui_transfer(state);
    if (!prepared.ok || prepared.title != "kiko receive" || prepared.spec.mode != 1 ||
        prepared.spec.code != "abc234" || prepared.spec.output_dir != "/tmp/kiko-out" ||
        prepared.spec.relay.host != "relay.example" || prepared.spec.relay.port != 9000 ||
        !prepared.spec.relay_pass || *prepared.spec.relay_pass != "secret") {
      std::cerr << "FAIL: receive transfer spec was not prepared correctly\n";
      fs::remove(send_path);
      return 1;
    }
  }

  {
    TuiState transfer_state;
    bool woke = false;
    TuiReporter reporter(transfer_state, [&] { woke = true; });
    reporter.route_phase(RoutePhase::DirectProbing, RoutePhaseDetail{"trying direct", "default", true});
    if (!woke || transfer_state.route_phase_label != "direct connect (relay ready)" ||
        transfer_state.activity != "trying direct" || !contains(transfer_state.connectivity_log, "relay-ready")) {
      std::cerr << "FAIL: TUI reporter did not expose structured route phase\n";
      fs::remove(send_path);
      return 1;
    }
  }

  {
    TuiState transfer_state;
    bool woke = false;
    TuiReporter reporter(transfer_state, [&] { woke = true; });
    RouteTiming timing;
    timing.rendezvous_ms = 12;
    timing.direct_probe_ms = 34;
    timing.relay_commit_ms = 56;
    reporter.route_timing(timing);
    if (!woke ||
        transfer_state.route_timing_summary != "rendezvous=12ms direct_probe=34ms relay_commit=56ms" ||
        !contains(transfer_state.connectivity_log, "route timing: rendezvous=12ms direct_probe=34ms")) {
      std::cerr << "FAIL: TUI reporter did not expose route timing\n";
      fs::remove(send_path);
      return 1;
    }
  }

  {
    TuiState transfer_state;
    bool woke = false;
    transfer_state.current_file = "partial.bin";
    transfer_state.current_done = 100;
    transfer_state.current_size = 200;
    transfer_state.overall_done = 100;
    transfer_state.files_done = 1;
    transfer_state.handshake = true;
    TuiReporter reporter(transfer_state, [&] { woke = true; });
    reporter.transfer_retry(2, 3, "connection reset");
    reporter.transfer_retry_delay(2, 3, std::chrono::milliseconds(250));
    if (!woke || transfer_state.current_done != 0 || transfer_state.overall_done != 0 ||
        transfer_state.files_done != 0 || transfer_state.handshake ||
        transfer_state.route_phase_label != "reconnecting" ||
        transfer_state.activity != "waiting to reconnect 2/3" ||
        !contains(transfer_state.connectivity_log, "connection lost, retrying 2/3") ||
        !contains(transfer_state.connectivity_log, "reconnect in 250ms before attempt 2/3")) {
      std::cerr << "FAIL: TUI reporter did not reset progress for auto reconnect\n";
      fs::remove(send_path);
      return 1;
    }
  }

  {
    TuiState transfer_state;
    bool woke = false;
    TuiReporter reporter(transfer_state, [&] { woke = true; });
    reporter.file_start("partial.bin", 200);
    reporter.file_resume("partial.bin", 100, 200);
    if (!woke || transfer_state.activity != "resuming partial.bin" ||
        !contains(transfer_state.connectivity_log, "resume: partial.bin from 100/200 bytes")) {
      std::cerr << "FAIL: TUI reporter did not expose resume progress\n";
      fs::remove(send_path);
      return 1;
    }
  }

  {
    TuiState transfer_state;
    bool woke = false;
    TuiReporter reporter(transfer_state, [&] { woke = true; });
    ReceivePlanSummary summary;
    summary.item_count = 4;
    summary.total_bytes = 4096;
    summary.resume_count = 1;
    summary.resume_bytes = 1024;
    summary.skip_count = 1;
    summary.skip_bytes = 512;
    summary.rename_count = 1;
    summary.overwrite_count = 1;
    reporter.receive_plan(summary);
    const auto rendered = render_transfer_text(transfer_state);
    if (!woke || !transfer_state.has_receive_plan ||
        !contains(transfer_state.connectivity_log, "receive plan: 4 item(s), 4096 bytes") ||
        !contains(rendered, "receive plan") || !contains(rendered, "resume:") ||
        !contains(rendered, "skip:") || !contains(rendered, "rename:") ||
        !contains(rendered, "overwrite:")) {
      std::cerr << "FAIL: TUI reporter did not render structured receive plan\n";
      fs::remove(send_path);
      return 1;
    }
  }

  {
    TuiState transfer_state;
    int wakes = 0;
    TuiReporter reporter(transfer_state, [&] { ++wakes; });
    reporter.file_advance(10);
    reporter.file_advance(20);
    if (transfer_state.overall_done != 30 || transfer_state.current_done != 30 || wakes != 1) {
      std::cerr << "FAIL: TUI reporter did not throttle progress wakeups while keeping byte totals\n";
      fs::remove(send_path);
      return 1;
    }
    reporter.file_complete("partial.bin", 30, true);
    if (wakes != 2 || transfer_state.files_done != 1) {
      std::cerr << "FAIL: TUI reporter throttled a file completion wakeup\n";
      fs::remove(send_path);
      return 1;
    }
  }

  {
    TuiState transfer_state;
    transfer_state.title = "kiko receive";
    transfer_state.start = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    TuiReporter reporter(transfer_state, [] {});
    constexpr std::uint64_t total = 1024 * 1024;
    reporter.transfer_overview(1, total);
    reporter.file_start("done.bin", total);
    reporter.file_advance(total);
    reporter.file_complete("done.bin", total, true);
    reporter.transfer_complete(1, total);

    const auto first_rate = line_containing(render_transfer_text(transfer_state), "throughput:");
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    const auto second_rate = line_containing(render_transfer_text(transfer_state), "throughput:");
    if (first_rate.empty() || first_rate != second_rate) {
      std::cerr << "FAIL: completed transfer throughput was not frozen\n";
      fs::remove(send_path);
      return 1;
    }
    reporter.file_advance(1);
    if (transfer_state.overall_done != total) {
      std::cerr << "FAIL: completed transfer accepted late byte progress\n";
      fs::remove(send_path);
      return 1;
    }
  }

  fs::remove(send_path);
  std::cout << "tui_menu_state_test ok\n";
  return 0;
}
