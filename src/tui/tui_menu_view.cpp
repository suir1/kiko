#include "tui_menu_view.hpp"

#include "tui_advanced.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include <memory>
#include <utility>
#include <vector>

namespace kiko {
namespace {

void apply_menu_preset(TuiMenuState& menu, int preset, std::string& menu_error, const std::function<void()>& wake) {
  apply_network_preset(preset, menu.network);
  menu.connections_text = std::to_string(menu.network.connections);
  menu_error.clear();
  if (wake) wake();
}

}  // namespace

ftxui::Component make_tui_menu_view(TuiMenuState& menu, const Endpoint& default_relay, std::string& menu_error,
                                    TuiMenuCallbacks callbacks) {
  using namespace ftxui;

  auto modes = std::make_shared<std::vector<std::string>>(std::vector<std::string>{"Send", "Receive", "Notepad"});
  auto summary_path = std::make_shared<std::string>();
  auto summary_cache = std::make_shared<std::string>();
  auto wake = callbacks.wake;
  auto mode_toggle = Toggle(modes.get(), &menu.mode);

  InputOption relay_opt;
  relay_opt.multiline = false;
  auto relay_input = Input(&menu.relay, "host:port", relay_opt);

  InputOption pass_opt;
  pass_opt.password = true;
  pass_opt.multiline = false;
  auto relay_pass_input = Input(&menu.relay_pass, "relay password (optional)", pass_opt);

  auto path_input = Input(&menu.path, "file or directory to send");
  auto code_input = Input(&menu.code, "pairing code");
  auto out_input = Input(&menu.output_dir, "output directory");

  auto pick_send_file = std::move(callbacks.pick_send_file);
  auto pick_send_directory = std::move(callbacks.pick_send_directory);
  auto browse_send_path = std::move(callbacks.browse_send_path);
  auto pick_output_directory = std::move(callbacks.pick_output_directory);
  auto browse_output_dir = std::move(callbacks.browse_output_dir);
  auto send_file_button = Button("File...", [pick = std::move(pick_send_file)] {
    if (pick) pick();
  });
  auto send_folder_button = Button("Folder...", [pick = std::move(pick_send_directory)] {
    if (pick) pick();
  });
  auto send_paths_button = Button("Browse...", [browse = std::move(browse_send_path)] {
    if (browse) browse();
  });
  auto output_folder_button = Button("Folder...", [pick = std::move(pick_output_directory)] {
    if (pick) pick();
  });
  auto output_paths_button = Button("Browse...", [browse = std::move(browse_output_dir)] {
    if (browse) browse();
  });
  auto send_picker_controls =
      Container::Horizontal({send_file_button, send_folder_button, send_paths_button});
  auto output_picker_controls = Container::Horizontal({output_folder_button, output_paths_button});
  auto doctor_button = Button("Network check", std::move(callbacks.network_check));

  auto preset_wan = Button("公网", [&, wake] { apply_menu_preset(menu, 0, menu_error, wake); });
  auto preset_wifi = Button("Wi-Fi", [&, wake] { apply_menu_preset(menu, 1, menu_error, wake); });
  auto preset_corp = Button("仅relay", [&, wake] { apply_menu_preset(menu, 2, menu_error, wake); });
  auto preset_debug = Button("调试", [&, wake] { apply_menu_preset(menu, 3, menu_error, wake); });
  auto preset_row = Container::Horizontal({preset_wan, preset_wifi, preset_corp, preset_debug});

  auto cb_lan = Checkbox("LAN discovery", &menu.network.lan_discover);
  auto cb_only_local = Checkbox("LAN relay only (--local)", &menu.network.only_local);
  auto cb_no_local = Checkbox("Disable LAN relay (--no-local)", &menu.network.disable_local);
  auto cb_no_direct = Checkbox("Skip direct connect (--no-direct)", &menu.network.no_direct);
  auto cb_udp_probe = Checkbox("UDP / STUN probe (--udp-probe)", &menu.network.udp_probe);
  auto cb_avoid_vpn = Checkbox("Avoid VPN interface (--avoid-vpn)", &menu.network.avoid_vpn);
  auto ip_input = Input(&menu.network.manual_ip, "manual IP (--ip)");
  auto bind_input = Input(&menu.network.bind_interface, "bind interface (e.g. en0)");
  auto proxy_input = Input(&menu.network.proxy_url, "proxy URL (http:// or socks5://)");
  auto cb_gitignore = Checkbox("Respect .gitignore when sending", &menu.network.use_gitignore);
  auto cb_auto_conn = Checkbox("Auto connection count (--auto-connections)", &menu.network.auto_connections);
  auto connections_input = Input(&menu.connections_text, "parallel connections (--connections)");
  auto cb_note_custom_host = Checkbox("Custom code host", &menu.note_custom_host);

  auto gitignore_maybe = Maybe(cb_gitignore, [&] { return menu.mode == 0; });
  auto auto_conn_maybe = Maybe(cb_auto_conn, [&] { return menu.mode == 0; });
  auto connections_maybe = Maybe(connections_input, [&] { return menu.mode == 0 && !menu.network.auto_connections; });

  auto advanced_inner = Container::Vertical({
      cb_lan,
      cb_only_local,
      cb_no_local,
      cb_no_direct,
      cb_udp_probe,
      cb_avoid_vpn,
      ip_input,
      bind_input,
      proxy_input,
      gitignore_maybe,
      auto_conn_maybe,
      connections_maybe,
  });
  auto advanced_section = Collapsible("Advanced", advanced_inner, &menu.network.advanced_open);

  auto start_button = Button("Start", std::move(callbacks.start_transfer));

  auto note_custom_host_maybe = Maybe(cb_note_custom_host, [&] { return menu.mode == 2; });
  auto path_input_maybe = Maybe(path_input, [&] { return menu.mode == 0; });
  auto send_picker_maybe = Maybe(send_picker_controls, [&] { return menu.mode == 0; });
  auto out_input_maybe = Maybe(out_input, [&] { return menu.mode == 1; });
  auto output_picker_maybe = Maybe(output_picker_controls, [&] { return menu.mode == 1; });

  auto layout = Container::Vertical({
      mode_toggle,
      relay_input,
      relay_pass_input,
      path_input_maybe,
      send_picker_maybe,
      code_input,
      note_custom_host_maybe,
      out_input_maybe,
      output_picker_maybe,
      preset_row,
      advanced_section,
      doctor_button,
      start_button,
  });

  auto root = Renderer(layout, [&, modes, summary_path, summary_cache, mode_toggle, relay_input, relay_pass_input,
                                path_input, send_file_button, send_folder_button, send_paths_button, code_input,
                                cb_note_custom_host, out_input, output_folder_button, output_paths_button, preset_wan,
                                preset_wifi, preset_corp, preset_debug, advanced_section, doctor_button,
                                start_button] {
    Elements rows;
    const std::string relay_kind = menu.relay == default_relay.to_string() ? "公网默认" : "自建";
    rows.push_back(text("kiko") | bold | hcenter);
    rows.push_back(separator());
    rows.push_back(hbox({text("mode:  "), mode_toggle->Render()}));
    rows.push_back(hbox({text("relay: "), relay_input->Render() | flex,
                         text(" (" + relay_kind + ")") | dim}));
    rows.push_back(hbox({text("pass:  "), relay_pass_input->Render() | flex}));
    if (menu.mode == 0) {
      rows.push_back(hbox({text("path:  "), path_input->Render() | flex}));
      rows.push_back(hbox({text("       "), send_file_button->Render(), send_folder_button->Render(),
                           send_paths_button->Render()}));
      if (*summary_path != menu.path) {
        *summary_path = menu.path;
        *summary_cache = summarize_path(menu.path);
      }
      if (!summary_cache->empty()) {
        rows.push_back(hbox({text("       "), text(*summary_cache) | dim}));
      }
      rows.push_back(hbox({text("code:  "), code_input->Render() | flex, text(" (optional)") | dim}));
    } else if (menu.mode == 1) {
      rows.push_back(hbox({text("code:  "), code_input->Render() | flex}));
      rows.push_back(hbox({text("out:   "), out_input->Render() | flex}));
      rows.push_back(hbox({text("       "), output_folder_button->Render(), output_paths_button->Render()}));
    } else {
      rows.push_back(hbox({text("code:  "), code_input->Render() | flex, text(" (empty hosts, filled joins)") | dim}));
      rows.push_back(hbox({text("       "), cb_note_custom_host->Render()}));
    }
    rows.push_back(separator());
    rows.push_back(hbox({text("preset: "), text(network_preset_label(menu.network.preset)) | color(Color::Cyan)}));
    rows.push_back(hbox({preset_wan->Render(), preset_wifi->Render(), preset_corp->Render(), preset_debug->Render()}));
    rows.push_back(advanced_section->Render());
    rows.push_back(separator());
    rows.push_back(hbox({doctor_button->Render(), text("  "), start_button->Render()}) | hcenter);
    if (!menu_error.empty()) {
      rows.push_back(text(menu_error) | color(Color::Red));
    }
    rows.push_back(text("Tab to move, Enter on Start; q/Esc to quit") | dim);
    if (relay_pass_from_env() && menu.relay_pass.empty()) {
      rows.push_back(text("hint: KIKO_RELAY_PASS is set and will be used") | dim);
    }
    return vbox(std::move(rows)) | border;
  });

  return root;
}

}  // namespace kiko
