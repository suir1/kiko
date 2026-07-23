#pragma once

#include "core/common.hpp"
#include "tui_menu_state.hpp"

#include <ftxui/component/component_base.hpp>

#include <functional>
#include <string>

namespace kiko {

struct TuiMenuCallbacks {
  std::function<void()> pick_send_file;
  std::function<void()> pick_send_directory;
  std::function<void()> browse_send_path;
  std::function<void()> pick_output_directory;
  std::function<void()> browse_output_dir;
  std::function<void()> network_check;
  std::function<void()> start_transfer;
  std::function<void()> wake;
};

[[nodiscard]] ftxui::Component make_tui_menu_view(TuiMenuState& menu, const Endpoint& default_relay,
                                                  std::string& menu_error, TuiMenuCallbacks callbacks);

}  // namespace kiko
