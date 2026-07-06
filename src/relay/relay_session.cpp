#include "relay_session.hpp"

#include "connect/encrypted_session.hpp"
#include "core/cancellation.hpp"
#include "core/pake.hpp"
#include "core/protocol.hpp"
#include "transfer/transfer.hpp"

#include <chrono>

namespace kiko {
namespace {

void apply_relay_pass_fields(Message& msg, const std::optional<std::string>& relay_pass) {
  if (relay_pass && !relay_pass->empty()) msg.fields["relay_pass"] = *relay_pass;
}

bool is_loopback_host(const std::string& host) {
  return host == "127.0.0.1" || host == "::1" || host == "localhost";
}

std::vector<TcpSocket> open_relay_mux_channels(TcpSocket primary, Role role, const Endpoint& active_relay,
                                               const std::string& room, int connections,
                                               const ConnectOptions& connect_options,
                                               const std::optional<std::string>& relay_pass,
                                               TransferCancellation* cancellation) {
  std::vector<TcpSocket> channels;
  channels.reserve(static_cast<std::size_t>(connections));
  channels.push_back(std::move(primary));
  if (cancellation) cancellation->track(channels.back());
  auto aux_options = connect_options;
  if (is_loopback_host(active_relay.host)) aux_options.bind_interface.clear();
  for (int k = 1; k < connections; ++k) {
    auto aux = connect_tcp(active_relay, std::chrono::seconds(5), aux_options,
                           cancellation ? cancellation->flag() : nullptr);
    if (!aux.valid()) throw KikoError("failed to open auxiliary relay connection");
    if (cancellation) cancellation->track(aux);
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
                           ProgressReporter& reporter, RouteTiming timing,
                           TransferCancellation* cancellation) {
  auto session =
      secure_encrypted_session(std::move(relay_channel), Role::Sender, code, "relay", timing, reporter, cancellation);
  relay_channel = std::move(session.channel);
  auto key = std::move(session.key);

  if (connections > 1) {
    reporter.status("opening " + std::to_string(connections) + " parallel relay connections");
    auto channels =
        open_relay_mux_channels(std::move(relay_channel), Role::Sender, active_relay, room_token(code), connections,
                                connect_options, relay_pass, cancellation);
    send_files_mux(channels, key, files, reporter);
    return;
  }

  send_files(relay_channel, key, files, reporter);
}

void receive_files_over_relay(TcpSocket relay_channel, const Endpoint& active_relay, const std::string& code,
                              int connections, const ConnectOptions& connect_options,
                              const std::optional<std::string>& relay_pass,
                              const std::filesystem::path& output_dir, ProgressReporter& reporter,
                              RouteTiming timing, ConflictPolicy conflict_policy,
                              TransferCancellation* cancellation) {
  auto session =
      secure_encrypted_session(std::move(relay_channel), Role::Receiver, code, "relay", timing, reporter, cancellation);
  relay_channel = std::move(session.channel);
  auto key = std::move(session.key);

  if (connections > 1) {
    reporter.status("opening " + std::to_string(connections) + " parallel relay connections");
    auto channels =
        open_relay_mux_channels(std::move(relay_channel), Role::Receiver, active_relay, room_token(code), connections,
                                connect_options, relay_pass, cancellation);
    receive_files_mux(channels, key, output_dir, reporter, conflict_policy);
    return;
  }

  receive_files(relay_channel, key, output_dir, reporter, conflict_policy);
}

}  // namespace kiko
