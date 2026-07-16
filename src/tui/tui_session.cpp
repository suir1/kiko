#include "tui_session.hpp"

#include "core/cancellation.hpp"
#include "tui_advanced.hpp"

namespace kiko {

std::thread start_tui_transfer(TuiTransferSpec spec, TuiState& state, std::function<void()> wake,
                               std::shared_ptr<TransferCancellation> cancellation) {
  return std::thread([spec = std::move(spec), &state, wake = std::move(wake),
                      cancellation = std::move(cancellation)]() mutable {
    TuiReporter reporter(state, wake);

    try {
      if (spec.mode == 0) {
        SendConfig config;
        config.file = spec.path;
        config.relay = spec.relay;
        config.code = spec.code;
        config.relay_pass = spec.relay_pass;
        config.show_qrcode = true;
        config.cancellation = cancellation;
        apply_network_options_to_send(config, spec.network);
        run_send(config, reporter);
      } else {
        RecvConfig config;
        config.code = spec.code;
        config.relay = spec.relay;
        config.output_dir = spec.output_dir;
        config.relay_pass = spec.relay_pass;
        config.cancellation = cancellation;
        apply_network_options_to_peer(config, spec.network);
        run_recv(config, reporter);
      }
    } catch (const std::exception& e) {
      std::lock_guard<std::mutex> lock(state.mutex);
      if (cancellation && cancellation->requested()) {
        state.finish_canceled();
      } else {
        state.finish_failed(e.what());
      }
      state.doctor_running = false;
    }

    wake();
  });
}

}  // namespace kiko
