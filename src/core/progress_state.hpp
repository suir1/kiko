#pragma once

#include "core/progress.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace kiko {

[[nodiscard]] std::string format_route_outcome_label(const RouteOutcome& outcome);

class TransferProgressState {
 public:
  explicit TransferProgressState(std::size_t max_log_lines);

  void reset();
  void append_log(const std::string& text);
  [[nodiscard]] std::string joined_logs() const;

  void status(const std::string& message);
  void connectivity_report(const std::string& report);
  void route_phase_changed(RoutePhase phase, const RoutePhaseDetail& detail);
  void route_selected(const RouteOutcome& outcome);
  void route_timing_recorded(const RouteTiming& timing);
  void handshake_completed();
  void pairing_code_ready(const std::string& pairing_code);
  void transfer_overview_received(std::size_t file_count, std::uint64_t total_bytes);
  void receive_plan_ready(const ReceivePlanSummary& summary);
  void file_started(const std::string& path, std::uint64_t size);
  [[nodiscard]] bool file_advanced(std::uint64_t bytes_delta);
  void file_resumed(const std::string& path, std::uint64_t offset, std::uint64_t size);
  void file_completed();
  void transfer_completed(std::size_t file_count, std::uint64_t total_bytes);
  void transfer_retrying(int next_attempt, int max_attempts, const std::string& reason);
  void transfer_retry_waiting(int next_attempt, int max_attempts, std::chrono::milliseconds delay);

  void finish_success(const std::string& final_activity);
  void finish_failed(const std::string& message);
  void finish_canceled();

  std::string activity;
  std::string code;
  std::string current_file;
  std::uint64_t current_done = 0;
  std::uint64_t current_size = 0;
  std::uint64_t overall_done = 0;
  std::uint64_t overall_total = 0;
  std::size_t files_done = 0;
  std::size_t files_total = 0;
  std::string route_phase;
  std::string route_summary;
  std::string route_timing;
  ReceivePlanSummary receive_plan;
  bool has_receive_plan = false;
  bool handshake = false;
  bool finished = false;
  bool failed = false;
  bool canceled = false;
  std::string error;
  std::vector<std::string> logs;
  std::chrono::steady_clock::time_point started{};
  std::optional<std::chrono::steady_clock::time_point> ended;

 private:
  std::size_t max_log_lines_;
};

// Projects ProgressReporter events into TransferProgressState. Front-ends only
// provide the synchronization and publication policy for each state mutation.
class ProgressStateReporter : public ProgressReporter {
 public:
  void status(const std::string& message) override;
  void connectivity_report(const std::string& report) override;
  void route_phase(RoutePhase phase, const RoutePhaseDetail& detail) override;
  void route_outcome(const RouteOutcome& outcome) override;
  void route_timing(const RouteTiming& timing) override;
  void handshake_ok() override;
  void code_ready(const std::string& code, bool show_qrcode = true) override;
  void transfer_overview(std::size_t file_count, std::uint64_t total_bytes) override;
  void receive_plan(const ReceivePlanSummary& summary) override;
  void file_start(const std::string& path, std::uint64_t size) override;
  void file_advance(std::uint64_t bytes_delta) override;
  void file_resume(const std::string& path, std::uint64_t offset, std::uint64_t size) override;
  void file_complete(const std::string& path, std::uint64_t size, bool verified) override;
  void transfer_complete(std::size_t file_count, std::uint64_t total_bytes) override;
  void transfer_retry(int next_attempt, int max_attempts, const std::string& reason) override;
  void transfer_retry_delay(int next_attempt, int max_attempts, std::chrono::milliseconds delay) override;

 protected:
  enum class UpdateKind {
    Immediate,
    Progress,
  };

  using StateMutation = std::function<bool(TransferProgressState&)>;

  // The mutation captures event arguments by reference and must be applied
  // synchronously before this call returns.
  virtual void update_progress_state(UpdateKind kind, const StateMutation& mutation) = 0;
};

}  // namespace kiko
