#include "tui_menu_state.hpp"
#include "tui_transfer_view.hpp"

#include "platform.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace {

bool contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
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

  fs::remove(send_path);
  std::cout << "tui_menu_state_test ok\n";
  return 0;
}
