#include "tui_session.hpp"

#include "core/cancellation.hpp"

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

std::thread start_tui_transfer(TuiTransferConfig config, TuiState& state, std::function<void()> wake,
                               std::shared_ptr<TransferCancellation> cancellation) {
  return start_tui_task(
      [config = std::move(config)](ProgressReporter& reporter,
                                   const std::shared_ptr<TransferCancellation>& cancellation) mutable {
        if (auto* send = std::get_if<SendConfig>(&config)) {
          send->cancellation = cancellation;
          run_send(*send, reporter);
        } else {
          auto& recv = std::get<RecvConfig>(config);
          recv.cancellation = cancellation;
          run_recv(recv, reporter);
        }
      },
      state, std::move(wake), std::move(cancellation));
}

}  // namespace kiko
