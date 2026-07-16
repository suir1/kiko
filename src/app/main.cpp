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
#include <utility>

namespace {

constexpr int kDefaultPairTimeoutSec = static_cast<int>(kiko::kDefaultPairTimeout.count());

void apply_relay_pass_cli(std::optional<std::string>& pass, const std::string& cli_value,
                          const kiko::UserConfig& user_config) {
  if (!cli_value.empty()) pass = cli_value;
  else if (!pass) pass = kiko::resolve_relay_pass_default(user_config);
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

struct PeerCliOptions {
  explicit PeerCliOptions(std::string default_relay) : relay(std::move(default_relay)) {}

  std::string relay;
  std::string relay_pass;
  std::string listen = "[::]:0";
  bool no_direct = false;
  bool no_lan = false;
  bool no_local = false;
  bool only_local = false;
  bool udp_probe = false;
  bool avoid_vpn = false;
  std::string proxy;
  std::string manual_ip;
  std::string bind_interface;
  int pair_timeout_sec = kDefaultPairTimeoutSec;
};

void add_peer_connection_options(CLI::App* command, PeerCliOptions& options, bool hosts_local_relay) {
  command->add_option("--relay", options.relay, "Relay address");
  command->add_option("--relay-pass", options.relay_pass, "Relay password");
  command->add_option("--listen", options.listen, "Local listen address for direct connections");
  command->add_flag("--no-direct", options.no_direct, "Force relay path");
  command->add_flag("--no-lan", options.no_lan, "Disable LAN multicast discovery");
  command->add_flag("--no-local", options.no_local,
                    hosts_local_relay ? "Disable embedded LAN relay" : "Disable LAN relay candidates");
  command->add_flag("--local", options.only_local,
                    hosts_local_relay ? "Use only embedded LAN relay" : "Use only LAN relay candidates");
  command->add_flag("--udp-probe", options.udp_probe, "Run STUN NAT probe during rendezvous");
  command->add_option("--proxy", options.proxy, "Proxy URL (http://host:port or socks5://host:port)");
  command->add_option("--ip", options.manual_ip, "Manual IP override for relay and advertised endpoints");
  command->add_option("--bind-interface", options.bind_interface,
                      "Bind outbound TCP sockets to an interface (for example en0)");
  command->add_flag("--avoid-vpn", options.avoid_vpn,
                    "Bind outbound TCP sockets to a non-VPN physical interface when possible");
  command
      ->add_option("--pair-timeout", options.pair_timeout_sec,
                   hosts_local_relay ? "Seconds to wait for a peer to enter the code"
                                     : "Seconds to wait for the host to appear")
      ->check(CLI::PositiveNumber);
}

void apply_peer_cli_options(kiko::PeerConnectionOptions& config, const PeerCliOptions& options,
                            const kiko::UserConfig& user_config) {
  config.relay = kiko::parse_endpoint(options.relay, 9000);
  config.listen = kiko::parse_bind_endpoint(options.listen, 0);
  config.no_direct = options.no_direct;
  config.lan_discover = !options.no_lan;
  config.disable_local = options.no_local;
  config.only_local = options.only_local;
  config.udp_probe = options.udp_probe;
  if (!options.proxy.empty()) config.proxy = kiko::parse_proxy_url(options.proxy);
  if (!options.manual_ip.empty()) config.manual_ip = options.manual_ip;
  config.bind_interface = options.bind_interface;
  config.avoid_vpn = options.avoid_vpn;
  config.pair_timeout = std::chrono::seconds(options.pair_timeout_sec);
  apply_relay_pass_cli(config.relay_pass, options.relay_pass, user_config);
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
  PeerCliOptions send_peer{relay_default};
  std::string send_code;
  bool send_no_gitignore = false;
  bool send_no_qrcode = false;
  bool send_ai_route = false;
  bool send_ai_route_plan_only = false;
  bool send_ai_route_connectivity_only = false;
  bool send_auto_connections = false;
  bool send_debug_route = false;
  bool send_remember = false;
  bool send_no_reconnect = false;
  std::string send_symlinks = "follow";
  int send_connections = 4;
  int send_reconnect_attempts = 3;
  bool send_tui = false;
  auto* send_cmd = app.add_subcommand("send", "Send a file or directory");
  send_cmd->add_option("path", send_path, "File or directory to send")->required();
  send_cmd->add_option("--code", send_code, "Pairing code (6-char auto if omitted; any string or croc-style label-secret)");
  add_peer_connection_options(send_cmd, send_peer, true);
  send_cmd->add_flag("--no-gitignore", send_no_gitignore, "Do not apply .gitignore when sending directories");
  send_cmd->add_option("--symlinks", send_symlinks, "Symlink handling: follow or preserve")
      ->check(CLI::IsMember({"follow", "preserve"}));
  send_cmd->add_flag("--no-qrcode", send_no_qrcode, "Do not print a terminal QR code for the pairing code");
  send_cmd->add_flag("--ai-route", send_ai_route, "Merge AI-suggested route plan with rules (400ms deadline)");
  send_cmd->add_flag("--ai-route-plan-only", send_ai_route_plan_only, "Print AI route plan without applying it");
  send_cmd->add_flag("--ai-route-connectivity-only", send_ai_route_connectivity_only,
                     "Let AI adjust connectivity only, not transfer connection count");
  send_cmd->add_flag("--debug-route", send_debug_route, "Print route diagnostics before transfer");
  send_cmd->add_option("--connections", send_connections, "Parallel relay connections")->check(CLI::PositiveNumber);
  send_cmd->add_flag("--auto-connections", send_auto_connections, "Pick connection count from relay RTT and file size");
  send_cmd->add_flag("--no-reconnect", send_no_reconnect, "Disable automatic reconnect/resume after connection loss");
  send_cmd->add_option("--reconnect-attempts", send_reconnect_attempts, "Total transfer attempts including the first")
      ->check(CLI::PositiveNumber);
  send_cmd->add_flag("--remember", send_remember, "Save relay and path to ~/.config/kiko/config.json");
  send_cmd->add_flag("--tui", send_tui, "Show live progress UI");

  std::string recv_code;
  PeerCliOptions recv_peer{relay_default};
  std::string recv_out = ".";
  bool recv_ai_route = false;
  bool recv_ai_route_plan_only = false;
  bool recv_debug_route = false;
  bool recv_remember = false;
  bool recv_no_reconnect = false;
  std::string recv_on_conflict = "overwrite";
  int recv_reconnect_attempts = 3;
  bool recv_tui = false;
  auto* recv_cmd = app.add_subcommand("recv", "Receive files with a pairing code");
  recv_cmd->add_option("code", recv_code, "Pairing code from sender")->required();
  recv_cmd->add_option("--out", recv_out, "Output directory");
  add_peer_connection_options(recv_cmd, recv_peer, false);
  recv_cmd->add_flag("--ai-route", recv_ai_route, "Merge AI-suggested route plan with rules (400ms deadline)");
  recv_cmd->add_flag("--ai-route-plan-only", recv_ai_route_plan_only, "Print AI route plan without applying it");
  recv_cmd->add_option("--on-conflict", recv_on_conflict, "Existing file policy: overwrite, skip, or rename")
      ->check(CLI::IsMember({"overwrite", "skip", "rename"}));
  recv_cmd->add_flag("--debug-route", recv_debug_route, "Print route diagnostics before transfer");
  recv_cmd->add_flag("--no-reconnect", recv_no_reconnect, "Disable automatic reconnect/resume after connection loss");
  recv_cmd->add_option("--reconnect-attempts", recv_reconnect_attempts, "Total transfer attempts including the first")
      ->check(CLI::PositiveNumber);
  recv_cmd->add_flag("--remember", recv_remember, "Save relay and output directory to ~/.config/kiko/config.json");
  recv_cmd->add_flag("--tui", recv_tui, "Show live progress UI");

  std::string note_host_code;
  PeerCliOptions note_host_peer{relay_default};
  bool note_host_no_qrcode = false;
  bool note_host_tui = false;

  std::string note_join_code;
  PeerCliOptions note_join_peer{relay_default};
  bool note_join_tui = false;

  auto* note_cmd = app.add_subcommand("note", "Shared encrypted plaintext notepad");
  note_cmd->require_subcommand(1);
  auto* note_host_cmd = note_cmd->add_subcommand("host", "Host a shared notepad");
  note_host_cmd->add_option("--code", note_host_code, "Pairing code (auto if omitted)");
  add_peer_connection_options(note_host_cmd, note_host_peer, true);
  note_host_cmd->add_flag("--no-qrcode", note_host_no_qrcode, "Do not print a terminal QR code for the pairing code");
  note_host_cmd->add_flag("--tui", note_host_tui, "Open the notepad terminal UI");

  auto* note_join_cmd = note_cmd->add_subcommand("join", "Join a shared notepad");
  note_join_cmd->add_option("code", note_join_code, "Pairing code from host")->required();
  add_peer_connection_options(note_join_cmd, note_join_peer, false);
  note_join_cmd->add_flag("--tui", note_join_tui, "Open the notepad terminal UI");

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& error) {
    return app.exit(error);
  }

  try {
    if (app.got_subcommand(tui_cmd)) {
      return kiko::run_tui_menu(kiko::parse_endpoint(tui_relay, 9000));
    }

    if (app.got_subcommand(web_cmd)) {
      kiko::WebOptions opts;
      opts.listen = kiko::parse_bind_endpoint(web_listen, 0);
      opts.relay = kiko::parse_endpoint(web_relay, 9000);
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
      return kiko::run_relay(kiko::parse_bind_endpoint(relay_listen, 9000), config);
    }

    if (app.got_subcommand(doctor_cmd)) {
      kiko::DoctorOptions opts;
      opts.relay = kiko::parse_endpoint(doctor_relay, 9000);
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
      apply_peer_cli_options(config, send_peer, user_config);
      config.file = send_path;
      config.code = send_code;
      config.use_gitignore = !send_no_gitignore;
      config.symlink_mode = parse_symlink_mode_option(send_symlinks);
      config.show_qrcode = !send_no_qrcode;
      config.debug_route = send_debug_route;
      config.ai_route = send_ai_route;
      config.ai_route_plan_only = send_ai_route_plan_only;
      config.ai_route_connectivity_only = send_ai_route_connectivity_only;
      config.connections = send_connections;
      config.auto_connections = send_auto_connections;
      config.auto_reconnect = !send_no_reconnect;
      config.reconnect_attempts = send_reconnect_attempts;
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
      apply_peer_cli_options(config, recv_peer, user_config);
      config.code = recv_code;
      config.output_dir = recv_out;
      config.conflict_policy = parse_conflict_policy_option(recv_on_conflict);
      config.debug_route = recv_debug_route;
      config.ai_route = recv_ai_route;
      config.ai_route_plan_only = recv_ai_route_plan_only;
      config.auto_reconnect = !recv_no_reconnect;
      config.reconnect_attempts = recv_reconnect_attempts;
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
      kiko::PeerSessionConfig config;
      bool note_tui = false;
      if (note_cmd->got_subcommand(note_host_cmd)) {
        apply_peer_cli_options(config, note_host_peer, user_config);
        config.role = kiko::Role::Sender;
        config.code = note_host_code;
        config.show_qrcode = !note_host_no_qrcode;
        note_tui = note_host_tui;
      } else if (note_cmd->got_subcommand(note_join_cmd)) {
        apply_peer_cli_options(config, note_join_peer, user_config);
        config.role = kiko::Role::Receiver;
        config.code = note_join_code;
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
