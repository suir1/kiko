#pragma once

#include "tui_menu_state.hpp"
#include "tui_transfer_view.hpp"

#include <ftxui/component/component_base.hpp>

#include <functional>
#include <string>

namespace kiko {

[[nodiscard]] ftxui::Component make_tui_transfer_actions(TuiState& state, TuiMenuState& menu,
                                                         std::string& action_notice,
                                                         std::function<void()> repeat_transfer,
                                                         std::function<void()> diagnose_transfer,
                                                         std::function<void()> return_to_menu,
                                                         std::function<void()> quit,
                                                         std::function<void()> wake);

}  // namespace kiko
