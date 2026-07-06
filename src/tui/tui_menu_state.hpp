#pragma once

#include "core/common.hpp"
#include "note/notepad.hpp"
#include "tui_advanced.hpp"
#include "tui_transfer_spec.hpp"

#include <optional>
#include <string>

namespace kiko {

struct TuiMenuState {
  int mode = 0;
  int note_role = 0;
  std::string relay;
  std::string relay_pass;
  std::string path;
  std::string code;
  std::string output_dir = ".";
  TuiNetworkOptions network;
  std::string connections_text;
};

struct PathSummary {
  bool ok = false;
  std::string text;
};

struct TuiPreparedTransfer {
  bool ok = false;
  std::string error;
  std::string title;
  TuiTransferSpec spec;
};

struct TuiPreparedNote {
  bool ok = false;
  std::string error;
  NoteConfig config;
};

[[nodiscard]] TuiMenuState load_tui_menu_state(const Endpoint& default_relay);
void save_tui_menu_state(const TuiMenuState& state);
[[nodiscard]] std::optional<std::string> apply_connections_text(TuiMenuState& state);
[[nodiscard]] TuiPreparedTransfer prepare_tui_transfer(TuiMenuState& state);
[[nodiscard]] TuiPreparedNote prepare_tui_note(TuiMenuState& state);

[[nodiscard]] std::optional<std::string> relay_pass_from_env();
[[nodiscard]] std::optional<std::string> resolve_relay_pass(const std::string& input);
[[nodiscard]] PathSummary summarize_path(const std::string& path_text);
[[nodiscard]] std::string relay_kind_label(const std::string& relay_text, const Endpoint& default_relay);

}  // namespace kiko
