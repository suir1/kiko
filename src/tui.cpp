#include "tui.hpp"

#include "platform.hpp"
#include "progress.hpp"
#include "tui_browser.hpp"
#include "tui_doctor_modal.hpp"
#include "tui_menu_state.hpp"
#include "tui_menu_view.hpp"
#include "tui_session.hpp"
#include "tui_transfer_actions.hpp"
#include "tui_transfer_view.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace kiko {
namespace {

int run_transfer_screen(const std::string& title, const std::function<void(ProgressReporter&)>& run) {
  using namespace ftxui;

  TuiState state;
  state.title = title;

  auto screen = ScreenInteractive::Fullscreen();
  TuiReporter reporter(state, [&] { screen.PostEvent(Event::Custom); });

  std::thread worker([&] {
    try {
      run(reporter);
    } catch (const std::exception& e) {
      std::lock_guard<std::mutex> lock(state.mutex);
      state.failed = true;
      state.finished = true;
      state.error_message = e.what();
      state.activity = "error";
    }
    screen.PostEvent(Event::Custom);
  });

  auto renderer = Renderer([&] {
    std::lock_guard<std::mutex> lock(state.mutex);
    return render_transfer_view(state);
  });

  auto with_events = CatchEvent(renderer, [&](Event event) {
    if (event == Event::Character('q') || event == Event::Escape) {
      screen.Exit();
      return true;
    }
    return false;
  });

  screen.Loop(with_events);
  worker.join();

  std::lock_guard<std::mutex> lock(state.mutex);
  if (state.failed) {
    std::cerr << "error: " << state.error_message << "\n";
    return 1;
  }
  return 0;
}

}  // namespace

int run_tui_send(const SendConfig& config) {
  if (!stdin_is_tty()) {
    CliReporter reporter;
    return run_send(config, reporter);
  }
  return run_transfer_screen("kiko send", [&](ProgressReporter& reporter) { run_send(config, reporter); });
}

int run_tui_recv(const RecvConfig& config) {
  if (!stdin_is_tty()) {
    CliReporter reporter;
    return run_recv(config, reporter);
  }
  return run_transfer_screen("kiko receive", [&](ProgressReporter& reporter) { run_recv(config, reporter); });
}

int run_tui_menu(const Endpoint& default_relay) {
  using namespace ftxui;

  if (!stdin_is_tty()) {
    std::cerr << "kiko tui requires an interactive terminal\n";
    return 2;
  }

  TuiMenuState menu = load_tui_menu_state(default_relay);
  std::string menu_error;

  int screen_tab = 0;  // 0 = menu, 1 = transfer
  TuiState transfer_state;
  std::thread worker;
  bool worker_started = false;
  std::string copy_notice;
  bool quit_confirm_pending = false;
  Endpoint last_transfer_relay = default_relay;
  std::optional<std::string> last_transfer_relay_pass;

  auto save_prefs_from_menu = [&]() { save_tui_menu_state(menu); };

  auto join_worker_if_needed = [&]() {
    if (worker_started && worker.joinable()) {
      worker.join();
      worker_started = false;
    }
  };

  auto screen = ScreenInteractive::Fullscreen();
  auto wake = [&] { screen.PostEvent(Event::Custom); };

  auto browse_send_path = [&] {
    const auto start_path = menu.path.empty() ? std::filesystem::current_path() : std::filesystem::path(menu.path);
    if (auto picked = run_tui_path_picker(start_path, TuiPickMode::FileOrDirectory)) {
      menu.path = picked->string();
      menu_error.clear();
    }
  };

  auto browse_output_dir = [&] {
    const auto start_path =
        menu.output_dir.empty() ? std::filesystem::current_path() : std::filesystem::path(menu.output_dir);
    if (auto picked = run_tui_path_picker(start_path, TuiPickMode::DirectoryOnly)) {
      menu.output_dir = picked->string();
      menu_error.clear();
    }
  };

  auto network_check = [&] {
    Endpoint relay_ep;
    try {
      relay_ep = parse_endpoint(menu.relay);
    } catch (const std::exception& e) {
      menu_error = std::string("invalid relay: ") + e.what();
      return;
    }
    show_doctor_modal(relay_ep, resolve_relay_pass(menu.relay_pass), menu.network.udp_probe);
  };

  auto begin_transfer = [&]() -> bool {
    menu_error.clear();
    auto prepared = prepare_tui_transfer(menu);
    if (!prepared.ok) {
      menu_error = std::move(prepared.error);
      return false;
    }

    join_worker_if_needed();

    last_transfer_relay = prepared.spec.relay;
    last_transfer_relay_pass = prepared.spec.relay_pass;

    transfer_state.title = prepared.title;
    reset_transfer_state(transfer_state);
    copy_notice.clear();
    quit_confirm_pending = false;

    worker = start_tui_transfer(std::move(prepared.spec), transfer_state, wake);
    worker_started = true;
    screen_tab = 1;
    save_prefs_from_menu();
    return true;
  };

  auto repeat_transfer = [&]() {
    join_worker_if_needed();
    reset_transfer_state(transfer_state);
    copy_notice.clear();
    quit_confirm_pending = false;
    begin_transfer();
    wake();
  };

  auto return_to_menu = [&]() {
    join_worker_if_needed();
    save_prefs_from_menu();
    screen_tab = 0;
    copy_notice.clear();
    quit_confirm_pending = false;
    wake();
  };

  auto start_transfer = [&] {
    begin_transfer();
    wake();
  };

  auto transfer_actions = make_tui_transfer_actions(
      transfer_state, copy_notice, [&] { repeat_transfer(); },
      [&] {
        show_doctor_modal(last_transfer_relay, last_transfer_relay_pass, menu.network.udp_probe);
        wake();
      },
      [&] { return_to_menu(); }, [&] { screen.Exit(); }, wake);

  auto menu_view = make_tui_menu_view(menu, default_relay, menu_error,
                                      {browse_send_path, browse_output_dir, network_check, start_transfer, wake});

  auto transfer_layout = Container::Vertical({transfer_actions.visible_actions});

  auto transfer_renderer = Renderer(transfer_layout, [&] {
    Element transfer_view;
    bool done = false;
    {
      std::lock_guard<std::mutex> lock(transfer_state.mutex);
      transfer_view = render_transfer_view(transfer_state, copy_notice, quit_confirm_pending);
      done = transfer_state.finished || transfer_state.failed;
    }
    Elements rows;
    rows.push_back(std::move(transfer_view));
    if (done) {
      rows.push_back(separator());
      rows.push_back(transfer_actions.actions->Render());
    }
    return vbox(std::move(rows)) | border;
  });

  auto tabs = Container::Tab({menu_view.root, transfer_renderer}, &screen_tab);

  auto with_events = CatchEvent(tabs, [&](Event event) {
    const bool quit_key = event == Event::Character('q');

    if (screen_tab == 1) {
      bool done = false;
      {
        std::lock_guard<std::mutex> lock(transfer_state.mutex);
        done = transfer_state.finished || transfer_state.failed;
      }
      if (!done && quit_key) {
        if (quit_confirm_pending) {
          save_prefs_from_menu();
          screen.Exit();
        } else {
          quit_confirm_pending = true;
        }
        return true;
      }
      if (!done && event == Event::Escape && quit_confirm_pending) {
        quit_confirm_pending = false;
        return true;
      }
      if (!done) return false;
    }

    if (quit_key || event == Event::Escape) {
      save_prefs_from_menu();
      screen.Exit();
      return true;
    }
    return false;
  });

  screen.Loop(with_events);

  join_worker_if_needed();

  std::lock_guard<std::mutex> lock(transfer_state.mutex);
  if (transfer_state.failed) {
    std::cerr << "error: " << transfer_state.error_message << "\n";
    return 1;
  }
  return 0;
}

}  // namespace kiko
