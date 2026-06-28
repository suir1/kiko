#pragma once

#include "core/common.hpp"
#include "tui_menu_state.hpp"

#include <ftxui/component/component_base.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace kiko {

namespace detail {

void invoke_active_browse_callback(int mode, const std::function<void()>& browse_send_path,
                                   const std::function<void()>& browse_output_dir);

}  // namespace detail

struct TuiMenuCallbacks {
  std::function<void()> browse_send_path;
  std::function<void()> browse_output_dir;
  std::function<void()> network_check;
  std::function<void()> start_transfer;
  std::function<void()> wake;
};

struct TuiMenuView {
  ftxui::Component root;
  std::shared_ptr<std::vector<std::string>> modes;
};

[[nodiscard]] TuiMenuView make_tui_menu_view(TuiMenuState& menu, const Endpoint& default_relay,
                                             std::string& menu_error, TuiMenuCallbacks callbacks);

}  // namespace kiko
