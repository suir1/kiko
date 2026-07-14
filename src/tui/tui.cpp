#include "tui.hpp"

#include "core/cancellation.hpp"
#include "platform/platform.hpp"
#include "core/progress.hpp"
#include "tui_browser.hpp"
#include "tui_doctor_modal.hpp"
#include "tui_failure_hint.hpp"
#include "tui_menu_state.hpp"
#include "tui_menu_view.hpp"
#include "tui_note.hpp"
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
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace kiko {
namespace {

int run_transfer_screen(
    const std::string& title,
    const std::function<void(ProgressReporter&, const std::shared_ptr<TransferCancellation>&)>& run) {
  using namespace ftxui;

  TuiState state;
  state.title = title;
  auto cancellation = std::make_shared<TransferCancellation>();

  auto screen = ScreenInteractive::Fullscreen();
  TuiReporter reporter(state, [&] { screen.PostEvent(Event::Custom); });

  std::thread worker([&] {
    try {
      run(reporter, cancellation);
    } catch (const std::exception& e) {
      std::lock_guard<std::mutex> lock(state.mutex);
      if (cancellation->requested()) {
        state.finish_canceled();
      } else {
        state.finish_failed(e.what());
      }
    }
    screen.PostEvent(Event::Custom);
  });

  auto renderer = Renderer([&] {
    std::lock_guard<std::mutex> lock(state.mutex);
    return render_transfer_view(state);
  });

  auto with_events = CatchEvent(renderer, [&](Event event) {
    if (event == Event::Character('q') || event == Event::Escape) {
      bool done = false;
      {
        std::lock_guard<std::mutex> lock(state.mutex);
        done = state.finished || state.failed;
        if (!done) state.activity = "canceling...";
      }
      if (done) {
        screen.Exit();
      } else {
        cancellation->request();
        screen.PostEvent(Event::Custom);
      }
      return true;
    }
    return false;
  });

  screen.Loop(with_events);
  worker.join();

  std::lock_guard<std::mutex> lock(state.mutex);
  if (state.failed) {
    std::cerr << "error: " << state.error << "\n";
    return 1;
  }
  if (state.canceled) return 130;
  return 0;
}

}  // namespace

int run_tui_send(const SendConfig& config) {
  if (!stdin_is_tty()) {
    CliReporter reporter;
    return run_send(config, reporter);
  }
  return run_transfer_screen("kiko send", [&](ProgressReporter& reporter,
                                              const std::shared_ptr<TransferCancellation>& cancellation) {
    auto run_config = config;
    run_config.cancellation = cancellation;
    run_send(run_config, reporter);
  });
}

int run_tui_recv(const RecvConfig& config) {
  if (!stdin_is_tty()) {
    CliReporter reporter;
    return run_recv(config, reporter);
  }
  return run_transfer_screen("kiko receive", [&](ProgressReporter& reporter,
                                                 const std::shared_ptr<TransferCancellation>& cancellation) {
    auto run_config = config;
    run_config.cancellation = cancellation;
    run_recv(run_config, reporter);
  });
}

namespace {

int run_tui_menu_screen(const Endpoint& default_relay, std::optional<NoteConfig>& note_request) {
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
  std::string action_notice;
  bool quit_confirm_pending = false;
  std::shared_ptr<TransferCancellation> transfer_cancellation;
  Endpoint last_transfer_relay = default_relay;
  std::optional<std::string> last_transfer_relay_pass;

  auto save_prefs_from_menu = [&]() { save_tui_menu_state(menu); };

  auto join_worker_if_needed = [&]() {
    if (worker_started && worker.joinable()) {
      worker.join();
      worker_started = false;
      transfer_cancellation.reset();
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
    action_notice.clear();
    quit_confirm_pending = false;
    transfer_cancellation = std::make_shared<TransferCancellation>();

    worker = start_tui_transfer(std::move(prepared.spec), transfer_state, wake, transfer_cancellation);
    worker_started = true;
    screen_tab = 1;
    save_prefs_from_menu();
    return true;
  };

  auto repeat_transfer = [&]() {
    join_worker_if_needed();
    reset_transfer_state(transfer_state);
    action_notice.clear();
    quit_confirm_pending = false;
    begin_transfer();
    wake();
  };

  auto return_to_menu = [&]() {
    join_worker_if_needed();
    save_prefs_from_menu();
    screen_tab = 0;
    action_notice.clear();
    quit_confirm_pending = false;
    wake();
  };

  auto start_transfer = [&] {
    begin_transfer();
    wake();
  };

  auto start_action = [&] {
    if (menu.mode != 2) {
      start_transfer();
      return;
    }
    menu_error.clear();
    auto prepared = prepare_tui_note(menu);
    if (!prepared.ok) {
      menu_error = std::move(prepared.error);
      wake();
      return;
    }
    save_prefs_from_menu();
    note_request = std::move(prepared.config);
    screen.Exit();
  };

  auto request_transfer_cancel = [&] {
    if (transfer_cancellation) transfer_cancellation->request();
    {
      std::lock_guard<std::mutex> lock(transfer_state.mutex);
      if (!transfer_state.finished && !transfer_state.failed) transfer_state.activity = "canceling...";
    }
    quit_confirm_pending = false;
    wake();
  };

  auto transfer_actions = make_tui_transfer_actions(
      transfer_state, menu, action_notice, [&] { repeat_transfer(); },
      [&] {
        show_doctor_modal(last_transfer_relay, last_transfer_relay_pass, menu.network.udp_probe);
        wake();
      },
      [&] { return_to_menu(); }, [&] { screen.Exit(); }, wake);

  auto menu_view = make_tui_menu_view(menu, default_relay, menu_error,
                                      {browse_send_path, browse_output_dir, network_check, start_action, wake});

  auto transfer_layout = Container::Vertical({transfer_actions.visible_actions});

  auto transfer_renderer = Renderer(transfer_layout, [&] {
    Element transfer_view;
    bool done = false;
    bool failed = false;
    std::optional<FailureRecoveryHint> recovery_hint;
    {
      std::lock_guard<std::mutex> lock(transfer_state.mutex);
      failed = transfer_state.failed;
      if (failed) recovery_hint = suggest_failure_recovery(transfer_state, menu);
      transfer_view =
          render_transfer_view(transfer_state, action_notice, quit_confirm_pending,
                               failed && recovery_hint ? &*recovery_hint : nullptr);
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
          request_transfer_cancel();
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
  if (note_request) return 0;

  std::lock_guard<std::mutex> lock(transfer_state.mutex);
  if (transfer_state.failed) {
    std::cerr << "error: " << transfer_state.error << "\n";
    return 1;
  }
  return 0;
}

}  // namespace

int run_tui_menu(const Endpoint& default_relay) {
  while (true) {
    std::optional<NoteConfig> note_request;
    const int menu_result = run_tui_menu_screen(default_relay, note_request);
    if (menu_result != 0 || !note_request) return menu_result;

    const int note_result = run_tui_note(*note_request);
    if (note_result != 0 && note_result != 130) return note_result;
  }
}

}  // namespace kiko
