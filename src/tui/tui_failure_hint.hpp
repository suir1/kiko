#pragma once

#include "tui_menu_state.hpp"
#include "tui_transfer_view.hpp"

#include <string>

namespace kiko {

struct FailureRecoveryHint {
  int preset = 2;
  bool avoid_vpn = false;
  std::string reason;
};

[[nodiscard]] FailureRecoveryHint suggest_failure_recovery(const TuiState& state, const TuiMenuState& menu);

void apply_failure_recovery(TuiMenuState& menu, const FailureRecoveryHint& hint);

}  // namespace kiko
