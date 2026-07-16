#include "tui_transfer_actions.hpp"

#include "tui_failure_hint.hpp"

#include <ftxui/component/component.hpp>

#include <mutex>
#include <utility>

namespace kiko {

ftxui::Component make_tui_transfer_actions(TuiState& state, TuiMenuState& menu, std::string& action_notice,
                                           std::function<void()> repeat_transfer,
                                           std::function<void()> diagnose_transfer,
                                           std::function<void()> return_to_menu, std::function<void()> quit,
                                           std::function<void()> wake) {
  using namespace ftxui;

  auto transfer_finished = [&state] {
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.finished && !state.failed && !state.canceled;
  };
  auto transfer_failed = [&state] {
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.failed;
  };
  auto transfer_finished_or_failed = [&state] {
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.finished || state.failed;
  };

  const auto repeat = std::move(repeat_transfer);
  auto again_button = Button("Again", repeat);
  auto retry_button = Button("Retry", repeat);
  auto fix_retry_button = Button("Fix & retry", [&state, &menu, &action_notice, repeat, wake] {
    FailureRecoveryHint hint;
    {
      std::lock_guard<std::mutex> lock(state.mutex);
      if (!state.failed) return;
      hint = suggest_failure_recovery(state, menu);
    }
    apply_failure_recovery(menu, hint);
    action_notice = std::string("applied preset: ") + network_preset_label(hint.preset);
    repeat();
    wake();
  });
  auto diagnose_button = Button("Diagnose", std::move(diagnose_transfer));
  auto menu_button = Button("Menu", std::move(return_to_menu));
  auto quit_button = Button("Quit", std::move(quit));

  auto again_maybe = Maybe(again_button, transfer_finished);
  auto retry_maybe = Maybe(retry_button, transfer_failed);
  auto fix_retry_maybe = Maybe(fix_retry_button, transfer_failed);
  auto diagnose_maybe = Maybe(diagnose_button, transfer_failed);

  auto actions = Container::Horizontal({
      again_maybe,
      fix_retry_maybe,
      retry_maybe,
      diagnose_maybe,
      menu_button,
      quit_button,
  });
  return Maybe(actions, transfer_finished_or_failed);
}

}  // namespace kiko
