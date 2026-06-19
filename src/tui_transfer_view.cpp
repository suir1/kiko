#include "tui_transfer_view.hpp"

#include "qrcode_print.hpp"
#include "tui_advanced.hpp"
#include "tui_failure_hint.hpp"

#include <ftxui/dom/elements.hpp>

#include <sstream>
#include <utility>

namespace kiko {
namespace {

void trim_log(std::string& log, std::size_t max_lines) {
  std::size_t lines = log.empty() ? 0 : 1;
  for (char c : log) {
    if (c == '\n') ++lines;
  }
  while (lines > max_lines) {
    const auto pos = log.find('\n');
    if (pos == std::string::npos) break;
    log.erase(0, pos + 1);
    --lines;
  }
}

void log_append(std::string& log, const std::string& line) {
  if (!log.empty()) log.push_back('\n');
  log += line;
  trim_log(log, 8);
}

bool starts_with(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

std::string text_after_prefix(const std::string& value, const std::string& prefix) {
  auto out = value.substr(prefix.size());
  if (!out.empty() && out.front() == ' ') out.erase(0, 1);
  return out;
}

std::string format_duration(double seconds) {
  if (seconds < 0) seconds = 0;
  const int total = static_cast<int>(seconds);
  if (total < 60) return std::to_string(total) + "s";
  const int minutes = total / 60;
  const int secs = total % 60;
  return std::to_string(minutes) + "m " + std::to_string(secs) + "s";
}

std::string connectivity_stage(const TuiState& state) {
  if (state.finished) return "complete";
  if (state.failed) return "failed";
  if (state.handshake && state.files_total > 0 && state.files_done < state.files_total) return "transferring";
  if (state.handshake) return "secure channel ready";

  const auto& activity = state.activity;
  const auto has = [&](const char* needle) { return activity.find(needle) != std::string::npos; };

  if (has("transfer")) return "transferring";
  if (has("handshake") || has("encrypted")) return "securing";
  if (has("relay")) return "relay rendezvous";
  if (has("punch") || has("direct")) return "direct connect";
  if (has("peer") || has("hello") || has("rendezvous") || has("waiting") || has("listening")) {
    return "rendezvous";
  }
  if (has("starting") || has("probe")) return "starting";
  return activity.empty() ? "starting" : activity;
}

std::string route_outcome_label(const RouteOutcome& outcome) {
  std::string label = "control=" + outcome.control_path + " data=" + outcome.data_path;
  if (!outcome.reason.empty()) label += " (" + outcome.reason + ")";
  if (outcome.data_path == "direct" && !outcome.direct_candidate_kind.empty()) {
    label += " via " + outcome.direct_candidate_kind;
    if (outcome.direct_elapsed_ms >= 0) label += " " + std::to_string(outcome.direct_elapsed_ms) + "ms";
  } else if (outcome.data_path == "relay" && !outcome.direct_attempted) {
    label += " direct=not_attempted";
  } else if (outcome.data_path == "relay" && !outcome.direct_failure_summary.empty()) {
    label += " direct=" + outcome.direct_failure_summary;
  }
  return label;
}

}  // namespace

std::string human_bytes(std::uint64_t bytes) {
  static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
  double value = static_cast<double>(bytes);
  int unit = 0;
  while (value >= 1024.0 && unit < 4) {
    value /= 1024.0;
    ++unit;
  }
  std::ostringstream oss;
  oss.setf(std::ios::fixed);
  oss.precision(value < 10 && unit > 0 ? 2 : (value < 100 && unit > 0 ? 1 : 0));
  oss << value << " " << units[unit];
  return oss.str();
}

std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) lines.push_back(line);
  return lines;
}

void reset_transfer_state(TuiState& state) {
  state.code.clear();
  state.qrcode.clear();
  state.connectivity_log.clear();
  state.outbound_summary.clear();
  state.outbound_probe_summary.clear();
  state.route_plan_summary.clear();
  state.transfer_path_summary.clear();
  state.activity = "starting...";
  state.current_file.clear();
  state.current_done = 0;
  state.current_size = 0;
  state.overall_done = 0;
  state.overall_total = 0;
  state.files_total = 0;
  state.files_done = 0;
  state.handshake = false;
  state.finished = false;
  state.failed = false;
  state.error_message.clear();
  state.doctor_summary.clear();
  state.doctor_running = false;
  state.start = std::chrono::steady_clock::now();
}

TuiReporter::TuiReporter(TuiState& state, std::function<void()> wake)
    : state_(state), wake_(std::move(wake)) {}

void TuiReporter::status(const std::string& message) {
  {
    std::lock_guard<std::mutex> lock(state_.mutex);
    log_append(state_.connectivity_log, message);
    update_network_summary(message);
    state_.activity = message;
  }
  wake_();
}

void TuiReporter::connectivity_report(const std::string& report) {
  {
    std::lock_guard<std::mutex> lock(state_.mutex);
    log_append(state_.connectivity_log, report);
    state_.activity = "connectivity probe finished";
  }
  wake_();
}

void TuiReporter::route_outcome(const RouteOutcome& outcome) {
  {
    std::lock_guard<std::mutex> lock(state_.mutex);
    state_.transfer_path_summary = route_outcome_label(outcome);
    state_.activity = outcome.data_path == "direct" ? "direct TCP selected" : "relay TCP selected";
    log_append(state_.connectivity_log, "route outcome: " + state_.transfer_path_summary);
  }
  wake_();
}

void TuiReporter::handshake_ok() {
  {
    std::lock_guard<std::mutex> lock(state_.mutex);
    state_.handshake = true;
    state_.activity = "encrypted channel ready";
    log_append(state_.connectivity_log, "handshake ok (PAKE + XChaCha20-Poly1305)");
  }
  wake_();
}

void TuiReporter::code_ready(const std::string& code, bool show_qrcode) {
  {
    std::lock_guard<std::mutex> lock(state_.mutex);
    state_.code = code;
    state_.qrcode.clear();
    if (show_qrcode) {
      std::ostringstream oss;
      print_qrcode(oss, code);
      state_.qrcode = oss.str();
    }
  }
  wake_();
}

void TuiReporter::transfer_overview(std::size_t file_count, std::uint64_t total_bytes) {
  {
    std::lock_guard<std::mutex> lock(state_.mutex);
    state_.files_total = file_count;
    state_.overall_total = total_bytes;
    state_.activity = "transferring " + std::to_string(file_count) + " file(s)";
  }
  wake_();
}

void TuiReporter::file_start(const std::string& path, std::uint64_t size) {
  {
    std::lock_guard<std::mutex> lock(state_.mutex);
    state_.current_file = path;
    state_.current_size = size;
    state_.current_done = 0;
  }
  wake_();
}

void TuiReporter::file_advance(std::uint64_t bytes_delta) {
  {
    std::lock_guard<std::mutex> lock(state_.mutex);
    state_.current_done += bytes_delta;
    state_.overall_done += bytes_delta;
  }
  wake_();
}

void TuiReporter::file_complete(const std::string& path, std::uint64_t size, bool verified) {
  {
    std::lock_guard<std::mutex> lock(state_.mutex);
    (void)path;
    (void)size;
    (void)verified;
    ++state_.files_done;
  }
  wake_();
}

void TuiReporter::transfer_complete(std::size_t file_count, std::uint64_t total_bytes) {
  {
    std::lock_guard<std::mutex> lock(state_.mutex);
    state_.files_total = file_count;
    state_.files_done = file_count;
    state_.finished = true;
    state_.activity = "transfer complete";
    if (state_.overall_total == 0) state_.overall_total = total_bytes;
    state_.overall_done = total_bytes;
  }
  wake_();
}

void TuiReporter::update_network_summary(const std::string& message) {
  if (starts_with(message, "outbound probe:")) {
    state_.outbound_probe_summary = text_after_prefix(message, "outbound probe:");
  } else if (starts_with(message, "outbound interface:")) {
    state_.outbound_summary = text_after_prefix(message, "outbound interface:");
  } else if (starts_with(message, "outbound path:")) {
    state_.outbound_summary = text_after_prefix(message, "outbound path:");
  } else if (starts_with(message, "route plan:")) {
    state_.route_plan_summary = text_after_prefix(message, "route plan:");
  } else if (starts_with(message, "route result:")) {
    state_.transfer_path_summary = text_after_prefix(message, "route result:");
  } else if (message == "direct skipped, using relay" || message == "direct failed, using relay" ||
             message == "peer selected relay; using relay path") {
    state_.transfer_path_summary = "relay (" + message + ")";
  } else if (starts_with(message, "opening ") && message.find(" parallel direct connections") != std::string::npos) {
    state_.transfer_path_summary = "direct (" + message + ")";
  } else if (starts_with(message, "parallel direct unavailable")) {
    state_.transfer_path_summary = "direct single-channel fallback";
  }
}

ftxui::Element render_transfer_view(const TuiState& state, const std::string& copy_notice,
                                    bool quit_confirm_pending, const FailureRecoveryHint* recovery_hint) {
  using namespace ftxui;

  const double overall_ratio = state.overall_total > 0
                                   ? static_cast<double>(state.overall_done) / static_cast<double>(state.overall_total)
                                   : (state.finished ? 1.0 : 0.0);
  const double file_ratio = state.current_size > 0
                                ? static_cast<double>(state.current_done) / static_cast<double>(state.current_size)
                                : 0.0;

  const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - state.start).count();
  const std::uint64_t rate = elapsed > 0.01 ? static_cast<std::uint64_t>(state.overall_done / elapsed) : 0;

  Elements left;
  left.push_back(text(state.title) | bold);
  left.push_back(separator());
  left.push_back(hbox({text("stage:    "), text(connectivity_stage(state)) | color(Color::Cyan)}));

  if (!state.code.empty()) {
    left.push_back(hbox({text("pairing code: "), text(state.code) | bold | color(Color::Yellow)}));
  }

  if (!state.outbound_summary.empty() || !state.outbound_probe_summary.empty() ||
      !state.route_plan_summary.empty() || !state.transfer_path_summary.empty()) {
    left.push_back(text("network") | underlined);
    if (!state.outbound_summary.empty()) {
      left.push_back(hbox({text("  outbound: "), text(state.outbound_summary) | color(Color::Cyan)}));
    }
    if (!state.outbound_probe_summary.empty()) {
      left.push_back(hbox({text("  probes:   "), text(state.outbound_probe_summary) | dim}));
    }
    if (!state.route_plan_summary.empty()) {
      left.push_back(hbox({text("  plan:     "), text(state.route_plan_summary)}));
    }
    if (!state.transfer_path_summary.empty()) {
      left.push_back(hbox({text("  path:     "), text(state.transfer_path_summary) | color(Color::GreenLight)}));
    }
  }

  if (!state.connectivity_log.empty()) {
    left.push_back(text("events") | underlined);
    for (const auto& line : split_lines(state.connectivity_log)) {
      left.push_back(text("  " + line) | dim);
    }
  }

  left.push_back(separator());
  left.push_back(hbox({text("activity: "), text(state.activity) |
                                              color(state.failed ? Color::Red : Color::GreenLight)}));
  left.push_back(hbox({text("secure:   "),
                       text(state.handshake ? "yes (XChaCha20-Poly1305)" : "negotiating...")}));
  left.push_back(separator());
  left.push_back(hbox({text("files: "),
                       text(std::to_string(state.files_done) + " / " + std::to_string(state.files_total))}));
  if (!state.current_file.empty()) {
    left.push_back(text("current: " + state.current_file));
    left.push_back(hbox({text("  "), gauge(static_cast<float>(file_ratio)) | flex,
                         text(" " + human_bytes(state.current_done) + " / " + human_bytes(state.current_size))}));
  }
  left.push_back(separator());
  left.push_back(text("overall"));
  left.push_back(hbox({text("  "), gauge(static_cast<float>(overall_ratio)) | flex,
                       text(" " + human_bytes(state.overall_done) + " / " + human_bytes(state.overall_total))}));
  left.push_back(hbox({text("throughput: "), text(human_bytes(rate) + "/s")}));
  left.push_back(hbox({text("elapsed:   "), text(format_duration(elapsed))}));
  if (!state.finished && !state.failed && rate > 0 && state.overall_total > state.overall_done) {
    const double eta =
        static_cast<double>(state.overall_total - state.overall_done) / static_cast<double>(rate);
    left.push_back(hbox({text("eta:       "), text("~" + format_duration(eta)) | dim}));
  }

  if (state.failed) {
    left.push_back(separator());
    left.push_back(text("error: " + state.error_message) | color(Color::Red));
    if (state.doctor_running) {
      left.push_back(text("diagnosis: running network check...") | dim);
    } else if (!state.doctor_summary.empty()) {
      left.push_back(text("diagnosis") | underlined);
      for (const auto& line : split_lines(state.doctor_summary)) {
        left.push_back(text("  " + line));
      }
    }
    if (recovery_hint != nullptr && !recovery_hint->reason.empty()) {
      left.push_back(separator());
      left.push_back(text("suggestion") | underlined);
      left.push_back(text("  " + recovery_hint->reason) | color(Color::Yellow));
      left.push_back(text("  preset: " + std::string(network_preset_label(recovery_hint->preset))) | dim);
    }
  }
  if (!copy_notice.empty()) {
    left.push_back(separator());
    left.push_back(text(copy_notice) | color(Color::GreenLight));
  }
  left.push_back(separator());
  if (state.finished || state.failed) {
    left.push_back(text("Tab to actions below") | dim);
  } else if (quit_confirm_pending) {
    left.push_back(text("quit? press q or Ctrl+C again, Esc to cancel") | color(Color::Yellow));
  } else {
    left.push_back(text("q / Ctrl+C: quit (confirm while transferring)") | dim);
  }

  Element body = vbox(std::move(left)) | flex;
  Elements root;
  if (!state.qrcode.empty()) {
    Elements qr_rows;
    for (const auto& line : split_lines(state.qrcode)) {
      qr_rows.push_back(text(line));
    }
    root.push_back(hbox({body, separator(), vbox(std::move(qr_rows)) | border}));
  } else {
    root.push_back(body);
  }

  return vbox(std::move(root)) | border;
}

}  // namespace kiko
