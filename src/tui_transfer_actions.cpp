#include "tui_transfer_actions.hpp"

#include "clipboard.hpp"
#include "tui_failure_hint.hpp"

#include <ftxui/component/component.hpp>

#include <mutex>
#include <utility>

namespace kiko {
namespace {

bool transfer_finished(TuiState& state) {
  std::lock_guard<std::mutex> lock(state.mutex);
  return state.finished && !state.failed && !state.canceled;
}

bool transfer_failed(TuiState& state) {
  std::lock_guard<std::mutex> lock(state.mutex);
  return state.failed;
}

bool transfer_has_code(TuiState& state) {
  std::lock_guard<std::mutex> lock(state.mutex);
  return (state.finished || state.failed || state.canceled) && !state.code.empty();
}

void copy_transfer_code(TuiState& state, std::string& copy_notice, const std::function<void()>& wake) {
  std::string code;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    code = state.code;
  }
  if (code.empty()) return;
  copy_notice = copy_to_clipboard(code) ? "copied pairing code to clipboard" : "clipboard unavailable";
  wake();
}

}  // namespace

bool transfer_finished_or_failed(TuiState& state) {
  std::lock_guard<std::mutex> lock(state.mutex);
  return state.finished || state.failed;
}

TuiTransferActions make_tui_transfer_actions(TuiState& state, TuiMenuState& menu, std::string& copy_notice,
                                             std::function<void()> repeat_transfer,
                                             std::function<void()> diagnose_transfer,
                                             std::function<void()> return_to_menu, std::function<void()> quit,
                                             std::function<void()> wake) {
  using namespace ftxui;

  auto copy_button = Button("Copy code", [&state, &copy_notice, wake] { copy_transfer_code(state, copy_notice, wake); });
  const auto repeat = std::move(repeat_transfer);
  auto again_button = Button("Again", repeat);
  auto retry_button = Button("Retry", repeat);
  auto fix_retry_button = Button("Fix & retry", [&state, &menu, &copy_notice, repeat, wake] {
    FailureRecoveryHint hint;
    {
      std::lock_guard<std::mutex> lock(state.mutex);
      if (!state.failed) return;
      hint = suggest_failure_recovery(state, menu);
    }
    apply_failure_recovery(menu, hint);
    copy_notice = std::string("applied preset: ") + network_preset_label(hint.preset);
    repeat();
    wake();
  });
  auto copy_cmd_button = Button("Copy cmd", [&menu, &copy_notice, wake] {
    const auto command = build_cli_command_from_menu(menu);
    copy_notice =
        copy_to_clipboard(command) ? "copied CLI command to clipboard" : "clipboard unavailable";
    wake();
  });
  auto diagnose_button = Button("Diagnose", std::move(diagnose_transfer));
  auto menu_button = Button("Menu", std::move(return_to_menu));
  auto quit_button = Button("Quit", std::move(quit));

  auto copy_maybe = Maybe(copy_button, [&state] { return transfer_has_code(state); });
  auto again_maybe = Maybe(again_button, [&state] { return transfer_finished(state); });
  auto retry_maybe = Maybe(retry_button, [&state] { return transfer_failed(state); });
  auto fix_retry_maybe = Maybe(fix_retry_button, [&state] { return transfer_failed(state); });
  auto copy_cmd_maybe = Maybe(copy_cmd_button, [&state] { return transfer_failed(state); });
  auto diagnose_maybe = Maybe(diagnose_button, [&state] { return transfer_failed(state); });

  auto actions = Container::Horizontal({
      copy_maybe,
      again_maybe,
      fix_retry_maybe,
      retry_maybe,
      copy_cmd_maybe,
      diagnose_maybe,
      menu_button,
      quit_button,
  });
  auto visible_actions = Maybe(actions, [&state] { return transfer_finished_or_failed(state); });
  return {actions, visible_actions};
}

}  // namespace kiko
