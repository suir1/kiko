#pragma once

#include "progress.hpp"

#include <ftxui/dom/elements.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace kiko {

struct FailureRecoveryHint;

struct TuiState {
  std::mutex mutex;
  std::string title;
  std::string code;
  std::string qrcode;
  std::string connectivity_log;
  std::string outbound_summary;
  std::string outbound_probe_summary;
  std::string route_plan_summary;
  std::string transfer_path_summary;
  std::string route_timing_summary;
  std::string route_phase_label;
  std::string activity = "starting...";
  std::string current_file;
  std::uint64_t current_done = 0;
  std::uint64_t current_size = 0;
  std::uint64_t overall_done = 0;
  std::uint64_t overall_total = 0;
  std::size_t files_total = 0;
  std::size_t files_done = 0;
  bool handshake = false;
  bool finished = false;
  bool failed = false;
  std::string error_message;
  std::string doctor_summary;
  bool doctor_running = false;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

class TuiReporter : public ProgressReporter {
 public:
  TuiReporter(TuiState& state, std::function<void()> wake);

  void status(const std::string& message) override;
  void connectivity_report(const std::string& report) override;
  void route_phase(RoutePhase phase, const RoutePhaseDetail& detail) override;
  void route_outcome(const RouteOutcome& outcome) override;
  void route_timing(const RouteTiming& timing) override;
  void handshake_ok() override;
  void code_ready(const std::string& code, bool show_qrcode = true) override;
  void transfer_overview(std::size_t file_count, std::uint64_t total_bytes) override;
  void file_start(const std::string& path, std::uint64_t size) override;
  void file_advance(std::uint64_t bytes_delta) override;
  void file_resume(const std::string& path, std::uint64_t offset, std::uint64_t size) override;
  void file_complete(const std::string& path, std::uint64_t size, bool verified) override;
  void transfer_complete(std::size_t file_count, std::uint64_t total_bytes) override;
  void transfer_retry(int next_attempt, int max_attempts, const std::string& reason) override;

 private:
  void update_network_summary(const std::string& message);

  TuiState& state_;
  std::function<void()> wake_;
};

[[nodiscard]] std::string human_bytes(std::uint64_t bytes);
[[nodiscard]] std::vector<std::string> split_lines(const std::string& text);

void reset_transfer_state(TuiState& state);

[[nodiscard]] ftxui::Element render_transfer_view(const TuiState& state, const std::string& copy_notice = {},
                                                  bool quit_confirm_pending = false,
                                                  const FailureRecoveryHint* recovery_hint = nullptr);

}  // namespace kiko
