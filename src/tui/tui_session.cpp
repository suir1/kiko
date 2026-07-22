#include "tui_session.hpp"

#include "core/cancellation.hpp"
#include "tui_advanced.hpp"

#include <optional>

namespace kiko {

std::thread start_tui_task(TuiTask task, TuiState& state, std::function<void()> wake,
                           std::shared_ptr<TransferCancellation> cancellation) {
  return std::thread([task = std::move(task), &state, wake = std::move(wake),
                      cancellation = std::move(cancellation)]() mutable {
    TuiReporter reporter(state, wake);
    std::optional<std::string> task_error;

    try {
      task(reporter, cancellation);
    } catch (const std::exception& e) {
      task_error = e.what();
    }

    {
      std::lock_guard<std::mutex> lock(state.mutex);
      state.doctor_running = false;
      const bool canceled = cancellation && cancellation->requested();
      if (task_error) {
        if (canceled) {
          state.finish_canceled();
        } else {
          state.finish_failed(*task_error);
        }
      } else if (!state.finished) {
        if (canceled) {
          state.finish_canceled();
        } else {
          state.finish_success("complete");
        }
      }
    }

    wake();
  });
}

std::thread start_tui_transfer(TuiTransferSpec spec, TuiState& state, std::function<void()> wake,
                               std::shared_ptr<TransferCancellation> cancellation) {
  return start_tui_task(
      [spec = std::move(spec)](ProgressReporter& reporter,
                               const std::shared_ptr<TransferCancellation>& cancellation) mutable {
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
      },
      state, std::move(wake), std::move(cancellation));
}

}  // namespace kiko
