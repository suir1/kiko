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

  load_network_options(saved, state.network);
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
  save_network_options(prefs, copy.network);
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
  if (auto code_error = validate_pairing_code_format(state.code, state.mode == 1)) {
    return {.error = *code_error};
  }
  if (auto connections_error = apply_connections_text(state)) {
    return {.error = *connections_error};
  }
  if (auto net_error = validate_network_options(state.network, state.mode)) {
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
  prepared.ok = true;
  prepared.title = state.mode == 0 ? "kiko send" : "kiko receive";
  prepared.spec.mode = state.mode;
  prepared.spec.path = state.path;
  prepared.spec.code = state.code;
  prepared.spec.output_dir = state.output_dir;
  prepared.spec.relay = relay_ep;
  prepared.spec.relay_pass = resolve_relay_pass(state.relay_pass);
  prepared.spec.network = state.network;
  return prepared;
}

TuiPreparedNote prepare_tui_note(TuiMenuState& state) {
  if (state.mode != 2) return {.error = "select Notepad first"};
  if (auto code_error = validate_pairing_code_format(state.code, state.note_role == 1)) {
    return {.error = *code_error};
  }
  if (auto net_error = validate_network_options(state.network, state.mode)) {
    return {.error = *net_error};
  }

  Endpoint relay_ep;
  try {
    relay_ep = parse_endpoint(state.relay);
  } catch (const std::exception& e) {
    return {.error = std::string("invalid relay: ") + e.what()};
  }

  TuiPreparedNote prepared;
  prepared.ok = true;
  prepared.config.role = state.note_role == 0 ? Role::Sender : Role::Receiver;
  prepared.config.code = state.code;
  prepared.config.relay = relay_ep;
  prepared.config.relay_pass = resolve_relay_pass(state.relay_pass);
  prepared.config.no_direct = state.network.no_direct;
  prepared.config.lan_discover = state.network.lan_discover;
  prepared.config.disable_local = state.network.disable_local;
  prepared.config.only_local = state.network.only_local;
  prepared.config.udp_probe = state.network.udp_probe;
  prepared.config.show_qrcode = false;
  prepared.config.manual_ip =
      state.network.manual_ip.empty() ? std::nullopt : std::optional<std::string>(state.network.manual_ip);
  prepared.config.bind_interface = state.network.bind_interface;
  prepared.config.avoid_vpn = state.network.avoid_vpn;
  prepared.config.app = "note";
  if (!state.network.proxy_url.empty()) {
    prepared.config.proxy = parse_proxy_url(state.network.proxy_url);
  }
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

PathSummary summarize_path(const std::string& path_text) {
  if (path_text.empty()) return {};
  std::error_code ec;
  const std::filesystem::path path(path_text);
  if (!std::filesystem::exists(path, ec)) return {};

  if (std::filesystem::is_regular_file(path, ec)) {
    return {true, "file, " + human_bytes(std::filesystem::file_size(path, ec))};
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
  if (file_count == 0) return {true, "empty directory"};
  return {true, std::to_string(file_count) + " file(s), " + human_bytes(total_bytes)};
}

std::string relay_kind_label(const std::string& relay_text, const Endpoint& default_relay) {
  return relay_text == default_relay.to_string() ? "公网默认" : "自建";
}

}  // namespace kiko
