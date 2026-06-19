#include "relay_session.hpp"

#include "pake.hpp"
#include "protocol.hpp"
#include "transfer.hpp"

#include <chrono>

namespace kiko {
namespace {

void apply_relay_pass_fields(Message& msg, const std::optional<std::string>& relay_pass) {
  if (relay_pass && !relay_pass->empty()) msg.fields["relay_pass"] = *relay_pass;
}

bool is_loopback_host(const std::string& host) {
  return host == "127.0.0.1" || host == "::1" || host == "localhost";
}

int elapsed_ms_since(std::chrono::steady_clock::time_point start) {
  const auto elapsed = std::chrono::steady_clock::now() - start;
  return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

std::vector<TcpSocket> open_relay_mux_channels(TcpSocket primary, Role role, const Endpoint& active_relay,
                                               const std::string& room, int connections,
                                               const ConnectOptions& connect_options,
                                               const std::optional<std::string>& relay_pass) {
  std::vector<TcpSocket> channels;
  channels.reserve(static_cast<std::size_t>(connections));
  channels.push_back(std::move(primary));
  auto aux_options = connect_options;
  if (is_loopback_host(active_relay.host)) aux_options.bind_interface.clear();
  for (int k = 1; k < connections; ++k) {
    auto aux = connect_tcp(active_relay, std::chrono::seconds(5), aux_options);
    if (!aux.valid()) throw KikoError("failed to open auxiliary relay connection");
    Message aux_hello{"hello",
                      {{"room", room},
                       {"role", role_name(role)},
                       {"aux", "1"},
                       {"conn_index", std::to_string(k)}}};
    apply_relay_pass_fields(aux_hello, relay_pass);
    send_message(aux, aux_hello);
    channels.push_back(std::move(aux));
  }
  return channels;
}

}  // namespace

void send_files_over_relay(TcpSocket relay_channel, const Endpoint& active_relay, const std::string& code,
                           int connections, const ConnectOptions& connect_options,
                           const std::optional<std::string>& relay_pass, const std::vector<FileEntry>& files,
                           ProgressReporter& reporter, RouteTiming timing) {
  reporter.route_phase(RoutePhase::Securing,
                       RoutePhaseDetail{"securing relay channel", "relay", /*relay_fallback_ready=*/true});
  const auto securing_start = std::chrono::steady_clock::now();
  auto key = perform_handshake(relay_channel, Role::Sender, code);
  timing.securing_ms = elapsed_ms_since(securing_start);
  reporter.route_timing(timing);
  reporter.handshake_ok();

  if (connections > 1) {
    reporter.status("opening " + std::to_string(connections) + " parallel relay connections");
    auto channels =
        open_relay_mux_channels(std::move(relay_channel), Role::Sender, active_relay, room_token(code), connections,
                                connect_options, relay_pass);
    send_files_mux(channels, key, files, reporter);
    return;
  }

  send_files(relay_channel, key, files, reporter);
}

void receive_files_over_relay(TcpSocket relay_channel, const Endpoint& active_relay, const std::string& code,
                              int connections, const ConnectOptions& connect_options,
                              const std::optional<std::string>& relay_pass,
                              const std::filesystem::path& output_dir, ProgressReporter& reporter,
                              RouteTiming timing) {
  reporter.route_phase(RoutePhase::Securing,
                       RoutePhaseDetail{"securing relay channel", "relay", /*relay_fallback_ready=*/true});
  const auto securing_start = std::chrono::steady_clock::now();
  auto key = perform_handshake(relay_channel, Role::Receiver, code);
  timing.securing_ms = elapsed_ms_since(securing_start);
  reporter.route_timing(timing);
  reporter.handshake_ok();

  if (connections > 1) {
    reporter.status("opening " + std::to_string(connections) + " parallel relay connections");
    auto channels =
        open_relay_mux_channels(std::move(relay_channel), Role::Receiver, active_relay, room_token(code), connections,
                                connect_options, relay_pass);
    receive_files_mux(channels, key, output_dir, reporter);
    return;
  }

  receive_files(relay_channel, key, output_dir, reporter);
}

}  // namespace kiko
