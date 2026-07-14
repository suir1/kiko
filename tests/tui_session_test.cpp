#include "tui/tui_session.hpp"

#include "core/cancellation.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>

namespace fs = std::filesystem;

int main() {
  using namespace kiko;

  TuiTransferSpec spec;
  spec.mode = 0;
  spec.path =
      (fs::temp_directory_path() / ("kiko_tui_session_missing_" +
                                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
          .string();
  spec.relay = Endpoint{"127.0.0.1", 9000};

  TuiState state;
  int wakes = 0;
  const auto cancellation = std::make_shared<TransferCancellation>();

  const auto start = std::chrono::steady_clock::now();
  auto worker = start_tui_transfer(spec, state, [&] { ++wakes; }, cancellation);
  worker.join();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

  std::lock_guard<std::mutex> lock(state.mutex);
  if (!state.failed || !state.finished || state.error.find("not a file or directory") == std::string::npos) {
    std::cerr << "FAIL: missing send path should fail the TUI transfer worker\n";
    return 1;
  }
  if (state.doctor_running || !state.doctor_summary.empty()) {
    std::cerr << "FAIL: failed TUI transfer should not auto-run doctor in the transfer worker\n";
    return 1;
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

  std::cout << "tui_session_test ok\n";
  return 0;
}
