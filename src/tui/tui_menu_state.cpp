#include "tui_menu_state.hpp"

#include "tui_transfer_view.hpp"
#include "platform/user_config.hpp"

#include <cstdlib>
#include <filesystem>

namespace kiko {

TuiMenuState load_tui_menu_state(const Endpoint& default_relay) {
  TuiMenuState state;
  state.relay = default_relay.to_string();
  if (auto env_pass = relay_pass_from_env()) state.relay_pass = *env_pass;

  const UserConfig saved = load_user_config();
  if (!saved.relay_pass.empty() && !relay_pass_from_env()) state.relay_pass = saved.relay_pass;
  if (!saved.last_send_path.empty()) state.path = saved.last_send_path;
  if (!saved.last_recv_out_dir.empty()) state.output_dir = saved.last_recv_out_dir;
  if (saved.last_mode == 0 || saved.last_mode == 1) state.mode = saved.last_mode;

  state.network = saved.network;
  if (state.network.connections < 1) state.network.connections = 4;
  state.connections_text = std::to_string(state.network.connections);
  return state;
}

void save_tui_menu_state(const TuiMenuState& state) {
  TuiMenuState copy = state;
  (void)apply_connections_text(copy);

  UserConfig prefs = load_user_config();
  prefs.relay = copy.relay;
  prefs.relay_pass = copy.relay_pass;
  prefs.last_send_path = copy.path;
  prefs.last_recv_out_dir = copy.output_dir;
  if (copy.mode == 0 || copy.mode == 1) prefs.last_mode = copy.mode;
  prefs.network = copy.network;
  save_user_config(prefs);
}

std::optional<std::string> apply_connections_text(TuiMenuState& state) {
  try {
    const int parsed = std::stoi(state.connections_text);
    if (parsed > 0) state.network.connections = parsed;
  } catch (...) {
    return "connections must be a number";
  }
  return std::nullopt;
}

TuiPreparedTransfer prepare_tui_transfer(TuiMenuState& state) {
  if (state.mode != 0 && state.mode != 1) {
    return {.error = "select Send or Receive for a file transfer"};
  }
  if (state.mode == 0 && state.path.empty()) {
    return {.error = "path is required — type a path or press Browse"};
  }
  const auto code = normalize_pairing_code(state.code);
  if (auto code_error = validate_pairing_code_format(code, state.mode == 1)) {
    return {.error = *code_error};
  }
  if (auto connections_error = apply_connections_text(state)) {
    return {.error = *connections_error};
  }
  if (auto net_error = validate_network_options(state.network)) {
    return {.error = *net_error};
  }

  Endpoint relay_ep;
  try {
    relay_ep = parse_endpoint(state.relay);
  } catch (const std::exception& e) {
    return {.error = std::string("invalid relay: ") + e.what()};
  }

  if (state.mode == 0) {
    std::error_code ec;
    if (!std::filesystem::exists(state.path, ec)) {
      return {.error = "path does not exist: " + state.path};
    }
  }

  TuiPreparedTransfer prepared;
  prepared.title = state.mode == 0 ? "kiko send" : "kiko receive";
  prepared.spec.mode = state.mode;
  prepared.spec.path = state.path;
  prepared.spec.code = code;
  prepared.spec.output_dir = state.output_dir;
  prepared.spec.relay = relay_ep;
  prepared.spec.relay_pass = resolve_relay_pass(state.relay_pass);
  prepared.spec.network = state.network;
  return prepared;
}

TuiPreparedNote prepare_tui_note(TuiMenuState& state) {
  if (state.mode != 2) return {.error = "select Notepad first"};
  const auto code = normalize_pairing_code(state.code);
  const bool join = !code.empty() && !state.note_custom_host;
  if (auto code_error = validate_pairing_code_format(code, join)) {
    return {.error = *code_error};
  }
  if (auto net_error = validate_network_options(state.network)) {
    return {.error = *net_error};
  }

  Endpoint relay_ep;
  try {
    relay_ep = parse_endpoint(state.relay);
  } catch (const std::exception& e) {
    return {.error = std::string("invalid relay: ") + e.what()};
  }

  TuiPreparedNote prepared;
  prepared.config.role = join ? Role::Receiver : Role::Sender;
  prepared.config.code = code;
  prepared.config.relay = relay_ep;
  prepared.config.relay_pass = resolve_relay_pass(state.relay_pass);
  apply_network_options_to_peer(prepared.config, state.network);
  prepared.config.show_qrcode = false;
  prepared.config.app = "note";
  return prepared;
}

std::optional<std::string> relay_pass_from_env() {
  if (const char* env = std::getenv("KIKO_RELAY_PASS")) {
    if (env[0] != '\0') return std::string(env);
  }
  return std::nullopt;
}

std::optional<std::string> resolve_relay_pass(const std::string& input) {
  if (!input.empty()) return input;
  return relay_pass_from_env();
}

std::string summarize_path(const std::string& path_text) {
  if (path_text.empty()) return {};
  std::error_code ec;
  const std::filesystem::path path(path_text);
  if (!std::filesystem::exists(path, ec)) return {};

  if (std::filesystem::is_regular_file(path, ec)) {
    return "file, " + human_bytes(std::filesystem::file_size(path, ec));
  }
  if (!std::filesystem::is_directory(path, ec)) return {};

  std::size_t file_count = 0;
  std::uint64_t total_bytes = 0;
  for (auto it = std::filesystem::recursive_directory_iterator(path, ec);
       it != std::filesystem::recursive_directory_iterator(); ++it) {
    if (ec) break;
    if (!it->is_regular_file(ec)) continue;
    ++file_count;
    total_bytes += std::filesystem::file_size(it->path(), ec);
  }
  if (file_count == 0) return "empty directory";
  return std::to_string(file_count) + " file(s), " + human_bytes(total_bytes);
}

}  // namespace kiko
