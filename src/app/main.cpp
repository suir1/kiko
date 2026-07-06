#include "core/common.hpp"
#include "core/cancellation.hpp"
#include "core/config.hpp"
#include "diagnostics/doctor.hpp"
#include "core/progress.hpp"
#include "core/proxy.hpp"
#include "relay/relay.hpp"

#include "relay/relay_server.hpp"
#include "transfer/transfer.hpp"
#include "note/notepad.hpp"
#include "tui/tui.hpp"
#include "tui/tui_note.hpp"
#include "platform/user_config.hpp"
#include "web/web.hpp"

#include <CLI/CLI.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {

constexpr int kDefaultPairTimeoutSec = static_cast<int>(kiko::kDefaultPairTimeout.count());

kiko::Endpoint parse_endpoint_option(const std::string& value, std::uint16_t default_port) {
  return kiko::parse_endpoint(value, default_port);
}

kiko::Endpoint parse_bind_endpoint_option(const std::string& value, std::uint16_t default_port) {
  return kiko::parse_bind_endpoint(value, default_port);
}

std::optional<std::string> default_relay_pass(const kiko::UserConfig& user_config) {
  return kiko::resolve_relay_pass_default(user_config);
}

void apply_relay_pass_cli(std::optional<std::string>& pass, const std::string& cli_value,
                          const kiko::UserConfig& user_config) {
  if (!cli_value.empty()) pass = cli_value;
  else if (!pass) pass = default_relay_pass(user_config);
}

kiko::SymlinkMode parse_symlink_mode_option(const std::string& value) {
  if (value == "preserve") return kiko::SymlinkMode::Preserve;
  return kiko::SymlinkMode::Follow;
}

kiko::ConflictPolicy parse_conflict_policy_option(const std::string& value) {
  if (value == "skip") return kiko::ConflictPolicy::Skip;
  if (value == "rename") return kiko::ConflictPolicy::Rename;
  return kiko::ConflictPolicy::Overwrite;
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"croc-like encrypted file transfer", "kiko"};
  app.set_version_flag("--version", std::string("kiko ") + kiko::kVersion);
  app.require_subcommand(1);

  const kiko::UserConfig user_config = kiko::load_user_config();
  const std::string relay_default = kiko::resolve_relay_default(user_config);

  std::string tui_relay = relay_default;
  auto* tui_cmd = app.add_subcommand("tui", "Interactive terminal UI");
  tui_cmd->add_option("--relay", tui_relay, "Default relay address");

  std::string web_relay = relay_default;
  std::string web_relay_pass;
  std::string web_listen = "127.0.0.1:0";
  bool web_no_open = false;
  auto* web_cmd = app.add_subcommand("web", "Local browser UI");
  web_cmd->add_option("--relay", web_relay, "Default relay address");
  web_cmd->add_option("--relay-pass", web_relay_pass, "Relay password");
  web_cmd->add_option("--listen", web_listen, "Loopback listen address");
  web_cmd->add_flag("--no-open", web_no_open, "Print the URL without opening a browser");

  std::string relay_listen = "[::]:9000";
  std::string relay_pass;
  int relay_room_ttl_sec = 3 * 3600;
  int relay_cleanup_sec = 600;
  auto* relay_cmd = app.add_subcommand("relay", "Run rendezvous relay");
  relay_cmd->add_option("--listen", relay_listen, "Listen address");
  relay_cmd->add_option("--pass", relay_pass, "Require relay password from clients");
  relay_cmd->add_option("--room-ttl", relay_room_ttl_sec, "Seconds before evicting unmatched peers")->check(CLI::PositiveNumber);
  relay_cmd->add_option("--room-cleanup-interval", relay_cleanup_sec, "Seconds between room TTL sweeps")
      ->check(CLI::PositiveNumber);

  std::string doctor_relay = relay_default;
  std::string doctor_relay_pass;
  bool doctor_udp_probe = false;
  bool doctor_json = false;
  bool doctor_ai_explain = false;
  bool doctor_avoid_vpn = false;
  std::string doctor_proxy;
  std::string doctor_bind_interface;
  auto* doctor_cmd = app.add_subcommand("doctor", "Network connectivity diagnostics");
  doctor_cmd->add_option("--relay", doctor_relay, "Relay to probe");
  doctor_cmd->add_option("--relay-pass", doctor_relay_pass, "Relay password");
  doctor_cmd->add_flag("--udp-probe", doctor_udp_probe, "Run STUN NAT probe (auto with --ai-explain)");
  doctor_cmd->add_flag("--json", doctor_json, "JSON output");
  doctor_cmd->add_flag("--ai-explain", doctor_ai_explain, "Use AI for human-readable explanation (BYOK)");
  doctor_cmd->add_option("--proxy", doctor_proxy, "Proxy URL (http://host:port or socks5://host:port)");
  doctor_cmd->add_option("--bind-interface", doctor_bind_interface, "Bind outbound TCP sockets to an interface (for example en0)");
  doctor_cmd->add_flag("--avoid-vpn", doctor_avoid_vpn, "Bind outbound TCP sockets to a non-VPN physical interface when possible");

  std::string send_path;
  std::string send_relay = relay_default;
  std::string send_relay_pass;
  std::string send_code;
  std::string send_listen = "[::]:0";
  bool send_no_direct = false;
  bool send_no_gitignore = false;
  bool send_no_lan = false;
  bool send_no_qrcode = false;
  bool send_no_local = false;
  bool send_only_local = false;
  bool send_udp_probe = false;
  bool send_ai_route = false;
  bool send_ai_route_plan_only = false;
  bool send_ai_route_connectivity_only = false;
  bool send_auto_connections = false;
  bool send_avoid_vpn = false;
  bool send_debug_route = false;
  bool send_remember = false;
  bool send_no_reconnect = false;
  std::string send_proxy;
  std::string send_ip;
  std::string send_bind_interface;
  std::string send_symlinks = "follow";
  int send_connections = 4;
  int send_reconnect_attempts = 3;
  int send_pair_timeout_sec = kDefaultPairTimeoutSec;
  bool send_tui = false;
  auto* send_cmd = app.add_subcommand("send", "Send a file or directory");
  send_cmd->add_option("path", send_path, "File or directory to send")->required();
  send_cmd->add_option("--relay", send_relay, "Relay address");
  send_cmd->add_option("--relay-pass", send_relay_pass, "Relay password");
  send_cmd->add_option("--code", send_code, "Pairing code (6-char auto if omitted; any string or croc-style label-secret)");
  send_cmd->add_option("--listen", send_listen, "Local listen address for direct connections");
  send_cmd->add_flag("--no-direct", send_no_direct, "Force relay path");
  send_cmd->add_flag("--no-gitignore", send_no_gitignore, "Do not apply .gitignore when sending directories");
  send_cmd->add_option("--symlinks", send_symlinks, "Symlink handling: follow or preserve")
      ->check(CLI::IsMember({"follow", "preserve"}));
  send_cmd->add_flag("--no-lan", send_no_lan, "Disable LAN multicast discovery");
  send_cmd->add_flag("--no-local", send_no_local, "Disable embedded LAN relay on sender");
  send_cmd->add_flag("--local", send_only_local, "Use only embedded LAN relay (no external relay)");
  send_cmd->add_flag("--no-qrcode", send_no_qrcode, "Do not print a terminal QR code for the pairing code");
  send_cmd->add_flag("--udp-probe", send_udp_probe, "Run STUN NAT probe during rendezvous (auto with --ai-route)");
  send_cmd->add_flag("--ai-route", send_ai_route, "Merge AI-suggested route plan with rules (400ms deadline)");
  send_cmd->add_flag("--ai-route-plan-only", send_ai_route_plan_only, "Print AI route plan without applying it");
  send_cmd->add_flag("--ai-route-connectivity-only", send_ai_route_connectivity_only,
                     "Let AI adjust connectivity only, not transfer connection count");
  send_cmd->add_option("--proxy", send_proxy, "Proxy URL (http://host:port or socks5://host:port)");
  send_cmd->add_option("--ip", send_ip, "Manual IP override for relay and advertised endpoints");
  send_cmd->add_option("--bind-interface", send_bind_interface,
                       "Bind outbound TCP sockets to an interface (for example en0)");
  send_cmd->add_flag("--avoid-vpn", send_avoid_vpn, "Bind outbound TCP sockets to a non-VPN physical interface when possible");
  send_cmd->add_flag("--debug-route", send_debug_route, "Print route diagnostics before transfer");
  send_cmd->add_option("--connections", send_connections, "Parallel relay connections")->check(CLI::PositiveNumber);
  send_cmd->add_flag("--auto-connections", send_auto_connections, "Pick connection count from relay RTT and file size");
  send_cmd->add_flag("--no-reconnect", send_no_reconnect, "Disable automatic reconnect/resume after connection loss");
  send_cmd->add_option("--reconnect-attempts", send_reconnect_attempts, "Total transfer attempts including the first")
      ->check(CLI::PositiveNumber);
  send_cmd->add_option("--pair-timeout", send_pair_timeout_sec, "Seconds to wait for the receiver to enter the code")
      ->check(CLI::PositiveNumber);
  send_cmd->add_flag("--remember", send_remember, "Save relay and path to ~/.config/kiko/config.json");
  send_cmd->add_flag("--tui", send_tui, "Show live progress UI");

  std::string recv_code;
  std::string recv_relay = relay_default;
  std::string recv_relay_pass;
  std::string recv_out = ".";
  std::string recv_listen = "[::]:0";
  bool recv_no_direct = false;
  bool recv_no_lan = false;
  bool recv_no_local = false;
  bool recv_only_local = false;
  bool recv_udp_probe = false;
  bool recv_ai_route = false;
  bool recv_ai_route_plan_only = false;
  bool recv_avoid_vpn = false;
  bool recv_debug_route = false;
  bool recv_remember = false;
  bool recv_no_reconnect = false;
  std::string recv_proxy;
  std::string recv_ip;
  std::string recv_bind_interface;
  std::string recv_on_conflict = "overwrite";
  int recv_reconnect_attempts = 3;
  int recv_pair_timeout_sec = kDefaultPairTimeoutSec;
  bool recv_tui = false;
  auto* recv_cmd = app.add_subcommand("recv", "Receive files with a pairing code");
  recv_cmd->add_option("code", recv_code, "Pairing code from sender")->required();
  recv_cmd->add_option("--relay", recv_relay, "Relay address");
  recv_cmd->add_option("--relay-pass", recv_relay_pass, "Relay password");
  recv_cmd->add_option("--out", recv_out, "Output directory");
  recv_cmd->add_option("--listen", recv_listen, "Local listen address for direct connections");
  recv_cmd->add_flag("--no-direct", recv_no_direct, "Force relay path");
  recv_cmd->add_flag("--no-lan", recv_no_lan, "Disable LAN multicast discovery");
  recv_cmd->add_flag("--no-local", recv_no_local, "Disable LAN relay candidates on receiver");
  recv_cmd->add_flag("--local", recv_only_local, "Use only LAN relay candidates (no external relay)");
  recv_cmd->add_flag("--udp-probe", recv_udp_probe, "Run STUN NAT probe during rendezvous (auto with --ai-route)");
  recv_cmd->add_flag("--ai-route", recv_ai_route, "Merge AI-suggested route plan with rules (400ms deadline)");
  recv_cmd->add_flag("--ai-route-plan-only", recv_ai_route_plan_only, "Print AI route plan without applying it");
  recv_cmd->add_option("--proxy", recv_proxy, "Proxy URL (http://host:port or socks5://host:port)");
  recv_cmd->add_option("--ip", recv_ip, "Manual IP override for relay and advertised endpoints");
  recv_cmd->add_option("--bind-interface", recv_bind_interface,
                       "Bind outbound TCP sockets to an interface (for example en0)");
  recv_cmd->add_option("--on-conflict", recv_on_conflict, "Existing file policy: overwrite, skip, or rename")
      ->check(CLI::IsMember({"overwrite", "skip", "rename"}));
  recv_cmd->add_flag("--avoid-vpn", recv_avoid_vpn, "Bind outbound TCP sockets to a non-VPN physical interface when possible");
  recv_cmd->add_flag("--debug-route", recv_debug_route, "Print route diagnostics before transfer");
  recv_cmd->add_flag("--no-reconnect", recv_no_reconnect, "Disable automatic reconnect/resume after connection loss");
  recv_cmd->add_option("--reconnect-attempts", recv_reconnect_attempts, "Total transfer attempts including the first")
      ->check(CLI::PositiveNumber);
  recv_cmd->add_option("--pair-timeout", recv_pair_timeout_sec, "Seconds to wait for the sender to enter the code")
      ->check(CLI::PositiveNumber);
  recv_cmd->add_flag("--remember", recv_remember, "Save relay and output directory to ~/.config/kiko/config.json");
  recv_cmd->add_flag("--tui", recv_tui, "Show live progress UI");

  std::string note_host_code;
  std::string note_host_relay = relay_default;
  std::string note_host_relay_pass;
  std::string note_host_listen = "[::]:0";
  bool note_host_no_direct = false;
  bool note_host_no_lan = false;
  bool note_host_no_local = false;
  bool note_host_only_local = false;
  bool note_host_udp_probe = false;
  bool note_host_avoid_vpn = false;
  bool note_host_no_qrcode = false;
  bool note_host_tui = false;
  int note_host_pair_timeout_sec = kDefaultPairTimeoutSec;
  std::string note_host_proxy;
  std::string note_host_ip;
  std::string note_host_bind_interface;

  std::string note_join_code;
  std::string note_join_relay = relay_default;
  std::string note_join_relay_pass;
  std::string note_join_listen = "[::]:0";
  bool note_join_no_direct = false;
  bool note_join_no_lan = false;
  bool note_join_no_local = false;
  bool note_join_only_local = false;
  bool note_join_udp_probe = false;
  bool note_join_avoid_vpn = false;
  bool note_join_tui = false;
  int note_join_pair_timeout_sec = kDefaultPairTimeoutSec;
  std::string note_join_proxy;
  std::string note_join_ip;
  std::string note_join_bind_interface;

  auto* note_cmd = app.add_subcommand("note", "Shared encrypted plaintext notepad");
  note_cmd->require_subcommand(1);
  auto* note_host_cmd = note_cmd->add_subcommand("host", "Host a shared notepad");
  note_host_cmd->add_option("--code", note_host_code, "Pairing code (auto if omitted)");
  note_host_cmd->add_option("--relay", note_host_relay, "Relay address");
  note_host_cmd->add_option("--relay-pass", note_host_relay_pass, "Relay password");
  note_host_cmd->add_option("--listen", note_host_listen, "Local listen address for direct connections");
  note_host_cmd->add_flag("--no-direct", note_host_no_direct, "Force relay path");
  note_host_cmd->add_flag("--no-lan", note_host_no_lan, "Disable LAN multicast discovery");
  note_host_cmd->add_flag("--no-local", note_host_no_local, "Disable embedded LAN relay on host");
  note_host_cmd->add_flag("--local", note_host_only_local, "Use only embedded LAN relay");
  note_host_cmd->add_flag("--udp-probe", note_host_udp_probe, "Run STUN NAT probe during rendezvous");
  note_host_cmd->add_option("--proxy", note_host_proxy, "Proxy URL (http://host:port or socks5://host:port)");
  note_host_cmd->add_option("--ip", note_host_ip, "Manual IP override for relay and advertised endpoints");
  note_host_cmd->add_option("--bind-interface", note_host_bind_interface,
                            "Bind outbound TCP sockets to an interface (for example en0)");
  note_host_cmd->add_flag("--avoid-vpn", note_host_avoid_vpn,
                          "Bind outbound TCP sockets to a non-VPN physical interface when possible");
  note_host_cmd->add_flag("--no-qrcode", note_host_no_qrcode, "Do not print a terminal QR code for the pairing code");
  note_host_cmd->add_option("--pair-timeout", note_host_pair_timeout_sec,
                            "Seconds to wait for a notepad peer to enter the code")
      ->check(CLI::PositiveNumber);
  note_host_cmd->add_flag("--tui", note_host_tui, "Open the notepad terminal UI");

  auto* note_join_cmd = note_cmd->add_subcommand("join", "Join a shared notepad");
  note_join_cmd->add_option("code", note_join_code, "Pairing code from host")->required();
  note_join_cmd->add_option("--relay", note_join_relay, "Relay address");
  note_join_cmd->add_option("--relay-pass", note_join_relay_pass, "Relay password");
  note_join_cmd->add_option("--listen", note_join_listen, "Local listen address for direct connections");
  note_join_cmd->add_flag("--no-direct", note_join_no_direct, "Force relay path");
  note_join_cmd->add_flag("--no-lan", note_join_no_lan, "Disable LAN multicast discovery");
  note_join_cmd->add_flag("--no-local", note_join_no_local, "Disable LAN relay candidates on join");
  note_join_cmd->add_flag("--local", note_join_only_local, "Use only LAN relay candidates");
  note_join_cmd->add_flag("--udp-probe", note_join_udp_probe, "Run STUN NAT probe during rendezvous");
  note_join_cmd->add_option("--proxy", note_join_proxy, "Proxy URL (http://host:port or socks5://host:port)");
  note_join_cmd->add_option("--ip", note_join_ip, "Manual IP override for relay and advertised endpoints");
  note_join_cmd->add_option("--bind-interface", note_join_bind_interface,
                            "Bind outbound TCP sockets to an interface (for example en0)");
  note_join_cmd->add_flag("--avoid-vpn", note_join_avoid_vpn,
                          "Bind outbound TCP sockets to a non-VPN physical interface when possible");
  note_join_cmd->add_option("--pair-timeout", note_join_pair_timeout_sec,
                            "Seconds to wait for the notepad host to appear")
      ->check(CLI::PositiveNumber);
  note_join_cmd->add_flag("--tui", note_join_tui, "Open the notepad terminal UI");

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& error) {
    return app.exit(error);
  }

  try {
    if (app.got_subcommand(tui_cmd)) {
      return kiko::run_tui_menu(parse_endpoint_option(tui_relay, 9000));
    }

    if (app.got_subcommand(web_cmd)) {
      kiko::WebOptions opts;
      opts.listen = parse_bind_endpoint_option(web_listen, 0);
      opts.relay = parse_endpoint_option(web_relay, 9000);
      apply_relay_pass_cli(opts.relay_pass, web_relay_pass, user_config);
      opts.open_browser = !web_no_open;
      opts.user_config = user_config;
      return kiko::run_web_console(opts);
    }

    if (app.got_subcommand(relay_cmd)) {
      kiko::RelayServerConfig config;
      config.password = relay_pass;
      config.room_ttl = std::chrono::seconds(relay_room_ttl_sec);
      config.cleanup_interval = std::chrono::seconds(relay_cleanup_sec);
      return kiko::run_relay(parse_bind_endpoint_option(relay_listen, 9000), config);
    }

    if (app.got_subcommand(doctor_cmd)) {
      kiko::DoctorOptions opts;
      opts.relay = parse_endpoint_option(doctor_relay, 9000);
      apply_relay_pass_cli(opts.relay_pass, doctor_relay_pass, user_config);
      opts.udp_probe = doctor_udp_probe;
      opts.json_output = doctor_json;
      opts.ai_explain = doctor_ai_explain;
      if (!doctor_proxy.empty()) opts.proxy = kiko::parse_proxy_url(doctor_proxy);
      opts.bind_interface = doctor_bind_interface;
      opts.avoid_vpn = doctor_avoid_vpn;
      return kiko::run_doctor_cli(opts);
    }

    if (app.got_subcommand(send_cmd)) {
      kiko::SendConfig config;
      config.file = send_path;
      config.relay = parse_endpoint_option(send_relay, 9000);
      config.code = send_code;
      config.listen = parse_bind_endpoint_option(send_listen, 0);
      config.no_direct = send_no_direct;
      config.use_gitignore = !send_no_gitignore;
      config.symlink_mode = parse_symlink_mode_option(send_symlinks);
      config.lan_discover = !send_no_lan;
      config.disable_local = send_no_local;
      config.only_local = send_only_local;
      config.show_qrcode = !send_no_qrcode;
      if (!send_proxy.empty()) config.proxy = kiko::parse_proxy_url(send_proxy);
      if (!send_ip.empty()) config.manual_ip = send_ip;
      config.bind_interface = send_bind_interface;
      config.avoid_vpn = send_avoid_vpn;
      config.debug_route = send_debug_route;
      apply_relay_pass_cli(config.relay_pass, send_relay_pass, user_config);
      config.udp_probe = send_udp_probe;
      config.ai_route = send_ai_route;
      config.ai_route_plan_only = send_ai_route_plan_only;
      config.ai_route_connectivity_only = send_ai_route_connectivity_only;
      config.connections = send_connections;
      config.auto_connections = send_auto_connections;
      config.auto_reconnect = !send_no_reconnect;
      config.reconnect_attempts = send_reconnect_attempts;
      config.pair_timeout = std::chrono::seconds(send_pair_timeout_sec);
      int rc = 0;
      if (send_tui) {
        rc = kiko::run_tui_send(config);
      } else {
        kiko::CliReporter reporter;
        rc = kiko::run_send(config, reporter);
      }
      if (rc == 0 && send_remember) {
        kiko::remember_send_settings(config.relay.to_string(), config.relay_pass, send_path);
      }
      return rc;
    }

    if (app.got_subcommand(recv_cmd)) {
      kiko::RecvConfig config;
      config.code = recv_code;
      config.relay = parse_endpoint_option(recv_relay, 9000);
      config.output_dir = recv_out;
      config.listen = parse_bind_endpoint_option(recv_listen, 0);
      config.no_direct = recv_no_direct;
      config.lan_discover = !recv_no_lan;
      config.disable_local = recv_no_local;
      config.only_local = recv_only_local;
      if (!recv_proxy.empty()) config.proxy = kiko::parse_proxy_url(recv_proxy);
      if (!recv_ip.empty()) config.manual_ip = recv_ip;
      config.bind_interface = recv_bind_interface;
      config.conflict_policy = parse_conflict_policy_option(recv_on_conflict);
      config.avoid_vpn = recv_avoid_vpn;
      config.debug_route = recv_debug_route;
      apply_relay_pass_cli(config.relay_pass, recv_relay_pass, user_config);
      config.udp_probe = recv_udp_probe;
      config.ai_route = recv_ai_route;
      config.ai_route_plan_only = recv_ai_route_plan_only;
      config.auto_reconnect = !recv_no_reconnect;
      config.reconnect_attempts = recv_reconnect_attempts;
      config.pair_timeout = std::chrono::seconds(recv_pair_timeout_sec);
      int rc = 0;
      if (recv_tui) {
        rc = kiko::run_tui_recv(config);
      } else {
        kiko::CliReporter reporter;
        rc = kiko::run_recv(config, reporter);
      }
      if (rc == 0 && recv_remember) {
        kiko::remember_recv_settings(config.relay.to_string(), config.relay_pass, recv_out);
      }
      return rc;
    }

    if (app.got_subcommand(note_cmd)) {
      kiko::NoteConfig config;
      bool note_tui = false;
      if (note_cmd->got_subcommand(note_host_cmd)) {
        config.role = kiko::Role::Sender;
        config.code = note_host_code;
        config.relay = parse_endpoint_option(note_host_relay, 9000);
        config.listen = parse_bind_endpoint_option(note_host_listen, 0);
        config.no_direct = note_host_no_direct;
        config.lan_discover = !note_host_no_lan;
        config.disable_local = note_host_no_local;
        config.only_local = note_host_only_local;
        config.udp_probe = note_host_udp_probe;
        config.show_qrcode = !note_host_no_qrcode;
        if (!note_host_proxy.empty()) config.proxy = kiko::parse_proxy_url(note_host_proxy);
        if (!note_host_ip.empty()) config.manual_ip = note_host_ip;
        config.bind_interface = note_host_bind_interface;
        config.avoid_vpn = note_host_avoid_vpn;
        config.pair_timeout = std::chrono::seconds(note_host_pair_timeout_sec);
        apply_relay_pass_cli(config.relay_pass, note_host_relay_pass, user_config);
        note_tui = note_host_tui;
      } else if (note_cmd->got_subcommand(note_join_cmd)) {
        config.role = kiko::Role::Receiver;
        config.code = note_join_code;
        config.relay = parse_endpoint_option(note_join_relay, 9000);
        config.listen = parse_bind_endpoint_option(note_join_listen, 0);
        config.no_direct = note_join_no_direct;
        config.lan_discover = !note_join_no_lan;
        config.disable_local = note_join_no_local;
        config.only_local = note_join_only_local;
        config.udp_probe = note_join_udp_probe;
        if (!note_join_proxy.empty()) config.proxy = kiko::parse_proxy_url(note_join_proxy);
        if (!note_join_ip.empty()) config.manual_ip = note_join_ip;
        config.bind_interface = note_join_bind_interface;
        config.avoid_vpn = note_join_avoid_vpn;
        config.pair_timeout = std::chrono::seconds(note_join_pair_timeout_sec);
        apply_relay_pass_cli(config.relay_pass, note_join_relay_pass, user_config);
        note_tui = note_join_tui;
      } else {
        return 2;
      }
      config.app = "note";
      config.cancellation = std::make_shared<kiko::TransferCancellation>();
      if (note_tui) return kiko::run_tui_note(config);
      kiko::CliReporter reporter;
      return kiko::run_note(config, reporter);
    }

    return 2;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << "\n";
    return 1;
  }
}
