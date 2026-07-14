#include "core/progress_state.hpp"

#include <sstream>

namespace kiko {
namespace {

void append_timing_field(std::string& line, const std::string& name, int value_ms) {
  if (value_ms < 0) return;
  if (!line.empty()) line += " ";
  line += name + "=" + std::to_string(value_ms) + "ms";
}

}  // namespace

std::string format_route_phase_label(RoutePhase phase, const RoutePhaseDetail& detail) {
  switch (phase) {
    case RoutePhase::Rendezvous:
      return "rendezvous";
    case RoutePhase::RelayStandby:
      return "relay fallback ready";
    case RoutePhase::DirectProbing:
      return detail.relay_fallback_ready ? "direct connect (relay ready)" : "direct connect";
    case RoutePhase::RelayCommitted:
      return "relay selected";
    case RoutePhase::Securing:
      return "securing";
  }
  return detail.message.empty() ? "starting" : detail.message;
}

std::string format_route_outcome_label(const RouteOutcome& outcome) {
  std::string label = "control=" + outcome.control_path + " data=" + outcome.data_path;
  if (!outcome.reason.empty()) label += " (" + outcome.reason + ")";
  if (outcome.data_path == "direct" && !outcome.direct_candidate_kind.empty()) {
    label += " via " + outcome.direct_candidate_kind;
    if (!outcome.direct_candidate_endpoint.empty()) label += " " + outcome.direct_candidate_endpoint;
    if (!outcome.direct_candidate_family.empty() && !outcome.direct_candidate_scope.empty()) {
      label += " " + outcome.direct_candidate_family + "/" + outcome.direct_candidate_scope;
    }
    if (outcome.direct_elapsed_ms >= 0) label += " " + std::to_string(outcome.direct_elapsed_ms) + "ms";
  } else if (outcome.data_path == "relay" && !outcome.direct_attempted) {
    label += " direct=not_attempted";
  } else if (outcome.data_path == "relay" && !outcome.direct_failure_summary.empty()) {
    label += " direct=" + outcome.direct_failure_summary;
  }
  return label;
}

std::string format_route_timing_label(const RouteTiming& timing) {
  std::string line;
  append_timing_field(line, "rendezvous", timing.rendezvous_ms);
  append_timing_field(line, "direct_probe", timing.direct_probe_ms);
  append_timing_field(line, "relay_commit", timing.relay_commit_ms);
  append_timing_field(line, "securing", timing.securing_ms);
  return line;
}

TransferProgressState::TransferProgressState(std::size_t max_log_lines)
    : max_log_lines_(max_log_lines) {
  reset();
}

void TransferProgressState::reset() {
  activity = "starting...";
  code.clear();
  current_file.clear();
  current_done = 0;
  current_size = 0;
  overall_done = 0;
  overall_total = 0;
  files_done = 0;
  files_total = 0;
  route_phase.clear();
  route_summary.clear();
  route_timing.clear();
  receive_plan = {};
  has_receive_plan = false;
  handshake = false;
  finished = false;
  failed = false;
  canceled = false;
  error.clear();
  logs.clear();
  started = std::chrono::steady_clock::now();
  ended.reset();
}

void TransferProgressState::append_log(const std::string& text) {
  std::istringstream input(text);
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty()) continue;
    logs.push_back(std::move(line));
  }
  if (max_log_lines_ > 0 && logs.size() > max_log_lines_) {
    logs.erase(logs.begin(), logs.begin() + static_cast<std::ptrdiff_t>(logs.size() - max_log_lines_));
  }
}

std::string TransferProgressState::joined_logs() const {
  std::string out;
  for (const auto& line : logs) {
    if (!out.empty()) out.push_back('\n');
    out += line;
  }
  return out;
}

void TransferProgressState::status(const std::string& message) {
  append_log(message);
  activity = message;
}

void TransferProgressState::connectivity_report(const std::string& report) {
  append_log(report);
  activity = "connectivity probe finished";
}

void TransferProgressState::route_phase_changed(RoutePhase phase, const RoutePhaseDetail& detail) {
  route_phase = format_route_phase_label(phase, detail);
  activity = detail.message.empty() ? route_phase : detail.message;
  std::string line = "route phase: " + route_phase;
  if (!detail.reason.empty()) line += " (" + detail.reason + ")";
  if (detail.relay_fallback_ready) line += " relay-ready";
  append_log(line);
}

void TransferProgressState::route_selected(const RouteOutcome& outcome) {
  route_summary = format_route_outcome_label(outcome);
  activity = outcome.data_path == "direct" ? "direct TCP selected" : "relay TCP selected";
  route_phase = activity;
  append_log("route outcome: " + route_summary);
}

void TransferProgressState::route_timing_recorded(const RouteTiming& timing) {
  const auto summary = format_route_timing_label(timing);
  if (summary.empty()) return;
  route_timing = summary;
  append_log("route timing: " + summary);
}

void TransferProgressState::handshake_completed() {
  handshake = true;
  activity = "encrypted channel ready";
  append_log("handshake ok (PAKE + XChaCha20-Poly1305)");
}

void TransferProgressState::pairing_code_ready(const std::string& pairing_code) {
  code = pairing_code;
}

void TransferProgressState::transfer_overview_received(std::size_t file_count, std::uint64_t total_bytes) {
  files_total = file_count;
  overall_total = total_bytes;
  activity = "transferring " + std::to_string(file_count) + " file(s)";
}

void TransferProgressState::receive_plan_ready(const ReceivePlanSummary& summary) {
  receive_plan = summary;
  has_receive_plan = true;
  overall_total = summary.total_bytes;
  activity = format_receive_plan_summary(summary);
  append_log(activity);
}

void TransferProgressState::file_started(const std::string& path, std::uint64_t size) {
  current_file = path;
  current_size = size;
  current_done = 0;
}

bool TransferProgressState::file_advanced(std::uint64_t bytes_delta) {
  if (finished || failed || canceled) return false;
  current_done += bytes_delta;
  overall_done += bytes_delta;
  return true;
}

void TransferProgressState::file_resumed(const std::string& path, std::uint64_t offset, std::uint64_t size) {
  activity = "resuming " + path;
  append_log("resume: " + path + " from " + std::to_string(offset) + "/" + std::to_string(size) + " bytes");
}

void TransferProgressState::file_completed() {
  ++files_done;
}

void TransferProgressState::transfer_completed(std::size_t file_count, std::uint64_t total_bytes) {
  files_total = file_count;
  files_done = file_count;
  overall_done = total_bytes;
  if (overall_total == 0) overall_total = total_bytes;
  finished = true;
  failed = false;
  canceled = false;
  activity = "transfer complete";
  ended = std::chrono::steady_clock::now();
}

void TransferProgressState::transfer_retrying(int next_attempt, int max_attempts, const std::string& reason) {
  current_file.clear();
  current_done = 0;
  current_size = 0;
  overall_done = 0;
  files_done = 0;
  handshake = false;
  finished = false;
  failed = false;
  canceled = false;
  error.clear();
  ended.reset();
  route_phase = "reconnecting";
  activity = "reconnecting " + std::to_string(next_attempt) + "/" + std::to_string(max_attempts);
  append_log(format_transfer_retry_summary(next_attempt, max_attempts, reason));
}

void TransferProgressState::transfer_retry_waiting(int next_attempt, int max_attempts,
                                                   std::chrono::milliseconds delay) {
  if (delay.count() <= 0) return;
  activity = "waiting to reconnect " + std::to_string(next_attempt) + "/" + std::to_string(max_attempts);
  append_log(format_transfer_retry_delay_summary(next_attempt, max_attempts, delay));
}

void TransferProgressState::finish_success(const std::string& final_activity) {
  finished = true;
  failed = false;
  canceled = false;
  error.clear();
  activity = final_activity;
  ended = std::chrono::steady_clock::now();
}

void TransferProgressState::finish_failed(const std::string& message) {
  append_log("error: " + message);
  finished = true;
  failed = true;
  canceled = false;
  error = message;
  activity = "failed";
  ended = std::chrono::steady_clock::now();
}

void TransferProgressState::finish_canceled() {
  append_log("canceled");
  finished = true;
  failed = false;
  canceled = true;
  error.clear();
  activity = "canceled";
  ended = std::chrono::steady_clock::now();
}

void ProgressStateReporter::status(const std::string& message) {
  update_progress_state(UpdateKind::Immediate, [&](TransferProgressState& state) {
    state.status(message);
    return true;
  });
}

void ProgressStateReporter::connectivity_report(const std::string& report) {
  update_progress_state(UpdateKind::Immediate, [&](TransferProgressState& state) {
    state.connectivity_report(report);
    return true;
  });
}

void ProgressStateReporter::route_phase(RoutePhase phase, const RoutePhaseDetail& detail) {
  update_progress_state(UpdateKind::Immediate, [&](TransferProgressState& state) {
    state.route_phase_changed(phase, detail);
    return true;
  });
}

void ProgressStateReporter::route_outcome(const RouteOutcome& outcome) {
  update_progress_state(UpdateKind::Immediate, [&](TransferProgressState& state) {
    state.route_selected(outcome);
    return true;
  });
}

void ProgressStateReporter::route_timing(const RouteTiming& timing) {
  update_progress_state(UpdateKind::Immediate, [&](TransferProgressState& state) {
    state.route_timing_recorded(timing);
    return true;
  });
}

void ProgressStateReporter::handshake_ok() {
  update_progress_state(UpdateKind::Immediate, [](TransferProgressState& state) {
    state.handshake_completed();
    return true;
  });
}

void ProgressStateReporter::code_ready(const std::string& code, bool show_qrcode) {
  (void)show_qrcode;
  update_progress_state(UpdateKind::Immediate, [&](TransferProgressState& state) {
    state.pairing_code_ready(code);
    return true;
  });
}

void ProgressStateReporter::transfer_overview(std::size_t file_count, std::uint64_t total_bytes) {
  update_progress_state(UpdateKind::Immediate, [&](TransferProgressState& state) {
    state.transfer_overview_received(file_count, total_bytes);
    return true;
  });
}

void ProgressStateReporter::receive_plan(const ReceivePlanSummary& summary) {
  update_progress_state(UpdateKind::Immediate, [&](TransferProgressState& state) {
    state.receive_plan_ready(summary);
    return true;
  });
}

void ProgressStateReporter::file_start(const std::string& path, std::uint64_t size) {
  update_progress_state(UpdateKind::Immediate, [&](TransferProgressState& state) {
    state.file_started(path, size);
    return true;
  });
}

void ProgressStateReporter::file_advance(std::uint64_t bytes_delta) {
  update_progress_state(UpdateKind::Progress,
                        [&](TransferProgressState& state) { return state.file_advanced(bytes_delta); });
}

void ProgressStateReporter::file_resume(const std::string& path, std::uint64_t offset,
                                        std::uint64_t size) {
  update_progress_state(UpdateKind::Immediate, [&](TransferProgressState& state) {
    state.file_resumed(path, offset, size);
    return true;
  });
}

void ProgressStateReporter::file_complete(const std::string& path, std::uint64_t size, bool verified) {
  (void)path;
  (void)size;
  (void)verified;
  update_progress_state(UpdateKind::Immediate, [](TransferProgressState& state) {
    state.file_completed();
    return true;
  });
}

void ProgressStateReporter::transfer_complete(std::size_t file_count, std::uint64_t total_bytes) {
  update_progress_state(UpdateKind::Immediate, [&](TransferProgressState& state) {
    state.transfer_completed(file_count, total_bytes);
    return true;
  });
}

void ProgressStateReporter::transfer_retry(int next_attempt, int max_attempts,
                                           const std::string& reason) {
  update_progress_state(UpdateKind::Immediate, [&](TransferProgressState& state) {
    state.transfer_retrying(next_attempt, max_attempts, reason);
    return true;
  });
}

void ProgressStateReporter::transfer_retry_delay(int next_attempt, int max_attempts,
                                                 std::chrono::milliseconds delay) {
  if (delay.count() <= 0) return;
  update_progress_state(UpdateKind::Immediate, [&](TransferProgressState& state) {
    state.transfer_retry_waiting(next_attempt, max_attempts, delay);
    return true;
  });
}

}  // namespace kiko
