#pragma once

#include "core/common.hpp"
#include "note/notepad.hpp"
#include "tui_advanced.hpp"
#include "tui_transfer_config.hpp"

#include <optional>
#include <string>

namespace kiko {

struct TuiMenuState {
  int mode = 0;
  bool note_custom_host = false;
  std::string relay;
  std::string relay_pass;
  std::string path;
  std::string code;
  std::string output_dir = ".";
  NetworkPreferences network;
  std::string connections_text;
};

struct TuiPreparedTransfer {
  std::string error;
  std::string title;
  TuiTransferConfig config;
};

struct TuiPreparedNote {
  std::string error;
  PeerSessionConfig config;
};

[[nodiscard]] TuiMenuState load_tui_menu_state(const Endpoint& default_relay);
void save_tui_menu_state(const TuiMenuState& state);
[[nodiscard]] std::optional<std::string> apply_connections_text(TuiMenuState& state);
[[nodiscard]] TuiPreparedTransfer prepare_tui_transfer(TuiMenuState& state);
[[nodiscard]] TuiPreparedNote prepare_tui_note(TuiMenuState& state);

[[nodiscard]] std::optional<std::string> relay_pass_from_env();
[[nodiscard]] std::optional<std::string> resolve_relay_pass(const std::string& input);
[[nodiscard]] std::string summarize_path(const std::string& path_text);

}  // namespace kiko
