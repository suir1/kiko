#include "tui/tui_session.hpp"

#include "core/cancellation.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace fs = std::filesystem;

int main() {
  using namespace kiko;

  SendConfig config;
  config.file =
      (fs::temp_directory_path() / ("kiko_tui_session_missing_" +
                                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
          .string();
  config.relay = Endpoint{"127.0.0.1", 9000};

  TuiState state;
  int wakes = 0;
  const auto cancellation = std::make_shared<TransferCancellation>();

  const auto start = std::chrono::steady_clock::now();
  auto worker = start_tui_transfer(TuiTransferConfig{std::move(config)}, state, [&] { ++wakes; }, cancellation);
  worker.join();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

  {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.failed || !state.finished || state.error.find("not a file or directory") == std::string::npos) {
      std::cerr << "FAIL: missing send path should fail the TUI transfer worker\n";
      return 1;
    }
    if (state.doctor_running || !state.doctor_summary.empty()) {
      std::cerr << "FAIL: failed TUI transfer should not auto-run doctor in the transfer worker\n";
      return 1;
    }
  }
  if (elapsed > std::chrono::milliseconds(250)) {
    std::cerr << "FAIL: failed TUI transfer worker took " << elapsed.count()
              << "ms; it may still be blocking on automatic doctor\n";
    return 1;
  }
  if (wakes == 0) {
    std::cerr << "FAIL: failed TUI transfer did not wake the UI\n";
    return 1;
  }

  TuiState canceled_state;
  canceled_state.doctor_running = true;
  int canceled_wakes = 0;
  const auto canceled = std::make_shared<TransferCancellation>();
  auto canceled_worker = start_tui_task(
      [](ProgressReporter&, const std::shared_ptr<TransferCancellation>& task_cancellation) {
        task_cancellation->request();
      },
      canceled_state, [&] { ++canceled_wakes; }, canceled);
  canceled_worker.join();

  {
    std::lock_guard<std::mutex> lock(canceled_state.mutex);
    if (!canceled_state.finished || !canceled_state.canceled || canceled_state.failed ||
        canceled_state.doctor_running) {
      std::cerr << "FAIL: canceled TUI task should map to canceled terminal state\n";
      return 1;
    }
  }
  if (canceled_wakes == 0) {
    std::cerr << "FAIL: canceled TUI task did not wake the UI\n";
    return 1;
  }

  TuiState completed_state;
  completed_state.doctor_running = true;
  int completed_wakes = 0;
  auto completed_worker = start_tui_task(
      [](ProgressReporter&, const std::shared_ptr<TransferCancellation>&) {}, completed_state,
      [&] { ++completed_wakes; }, std::make_shared<TransferCancellation>());
  completed_worker.join();

  {
    std::lock_guard<std::mutex> lock(completed_state.mutex);
    if (!completed_state.finished || completed_state.failed || completed_state.canceled ||
        completed_state.doctor_running || completed_state.activity != "complete") {
      std::cerr << "FAIL: completed TUI task should map to successful terminal state\n";
      return 1;
    }
  }
  if (completed_wakes == 0) {
    std::cerr << "FAIL: completed TUI task did not wake the UI\n";
    return 1;
  }

  TuiState late_failure_state;
  auto late_failure_worker = start_tui_task(
      [](ProgressReporter& reporter, const std::shared_ptr<TransferCancellation>&) {
        reporter.transfer_complete(1, 1);
        throw std::runtime_error("post-transfer failure");
      },
      late_failure_state, [] {}, std::make_shared<TransferCancellation>());
  late_failure_worker.join();

  {
    std::lock_guard<std::mutex> lock(late_failure_state.mutex);
    if (!late_failure_state.failed || late_failure_state.canceled ||
        late_failure_state.error != "post-transfer failure") {
      std::cerr << "FAIL: late TUI task failure should override an earlier progress completion\n";
      return 1;
    }
  }

  std::cout << "tui_session_test ok\n";
  return 0;
}
