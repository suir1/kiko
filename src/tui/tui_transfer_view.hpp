#pragma once

#include "core/progress_state.hpp"

#include <ftxui/dom/elements.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace kiko {

struct FailureRecoveryHint;

struct TuiState : TransferProgressState {
  TuiState() : TransferProgressState(8) {}

  std::mutex mutex;
  std::string title;
  std::string qrcode;
  std::string outbound_summary;
  std::string outbound_probe_summary;
  std::string route_plan_summary;
  std::string doctor_summary;
  bool doctor_running = false;
};

class TuiReporter : public ProgressStateReporter {
 public:
  TuiReporter(TuiState& state, std::function<void()> wake);

  void status(const std::string& message) override;
  void code_ready(const std::string& code, bool show_qrcode = true) override;

 protected:
  void update_progress_state(UpdateKind kind, const StateMutation& mutation) override;

 private:
  [[nodiscard]] bool should_wake_progress(std::chrono::steady_clock::time_point now);

  void update_network_summary(const std::string& message);

  TuiState& state_;
  std::function<void()> wake_;
  std::chrono::steady_clock::time_point last_progress_wake_{};
};

[[nodiscard]] std::string human_bytes(std::uint64_t bytes);
[[nodiscard]] std::vector<std::string> split_lines(const std::string& text);

void reset_transfer_state(TuiState& state);

[[nodiscard]] ftxui::Element render_transfer_view(const TuiState& state, const std::string& action_notice = {},
                                                  bool quit_confirm_pending = false,
                                                  const FailureRecoveryHint* recovery_hint = nullptr);

}  // namespace kiko
