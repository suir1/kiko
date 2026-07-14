#include "tui_session.hpp"

#include "core/cancellation.hpp"
#include "tui_advanced.hpp"

namespace kiko {
namespace {

SendConfig make_send_config(const TuiTransferSpec& spec, std::shared_ptr<TransferCancellation> cancellation) {
  SendConfig config;
  config.file = spec.path;
  config.relay = spec.relay;
  config.code = spec.code;
  config.relay_pass = spec.relay_pass;
  config.show_qrcode = true;
  config.cancellation = std::move(cancellation);
  apply_network_options_to_send(config, spec.network);
  return config;
}

RecvConfig make_recv_config(const TuiTransferSpec& spec, std::shared_ptr<TransferCancellation> cancellation) {
  RecvConfig config;
  config.code = spec.code;
  config.relay = spec.relay;
  config.output_dir = spec.output_dir;
  config.relay_pass = spec.relay_pass;
  config.cancellation = std::move(cancellation);
  apply_network_options_to_peer(config, spec.network);
  return config;
}

void mark_transfer_failed(TuiState& state, const std::exception& error) {
  std::lock_guard<std::mutex> lock(state.mutex);
  state.finish_failed(error.what());
  state.doctor_running = false;
}

void mark_transfer_canceled(TuiState& state) {
  std::lock_guard<std::mutex> lock(state.mutex);
  state.finish_canceled();
  state.doctor_running = false;
}

}  // namespace

std::thread start_tui_transfer(TuiTransferSpec spec, TuiState& state, std::function<void()> wake,
                               std::shared_ptr<TransferCancellation> cancellation) {
  return std::thread([spec = std::move(spec), &state, wake = std::move(wake),
                      cancellation = std::move(cancellation)]() mutable {
    TuiReporter reporter(state, wake);

    try {
      if (spec.mode == 0) {
        SendConfig config = make_send_config(spec, cancellation);
        run_send(config, reporter);
      } else {
        RecvConfig config = make_recv_config(spec, cancellation);
        run_recv(config, reporter);
      }
    } catch (const std::exception& e) {
      if (cancellation && cancellation->requested()) {
        mark_transfer_canceled(state);
      } else {
        mark_transfer_failed(state, e);
      }
    }

    wake();
  });
}

}  // namespace kiko
