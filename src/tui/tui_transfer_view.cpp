#include "tui_transfer_view.hpp"

#include "core/qrcode_print.hpp"
#include "tui_advanced.hpp"
#include "tui_failure_hint.hpp"

#include <ftxui/dom/elements.hpp>

#include <sstream>
#include <utility>

namespace kiko {
namespace {

constexpr auto kProgressWakeInterval = std::chrono::milliseconds(50);

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
  if (state.failed) return "failed";
  if (state.canceled) return "canceled";
  if (state.finished) return "complete";
  if (state.handshake && state.files_total > 0 && state.files_done < state.files_total) return "transferring";
  if (state.handshake) return "secure channel ready";
  if (!state.route_phase.empty()) return state.route_phase;

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
  state.reset();
  state.qrcode.clear();
  state.outbound_summary.clear();
  state.outbound_probe_summary.clear();
  state.route_plan_summary.clear();
  state.doctor_summary.clear();
  state.doctor_running = false;
}

TuiReporter::TuiReporter(TuiState& state, std::function<void()> wake)
    : state_(state), wake_(std::move(wake)) {}

void TuiReporter::status(const std::string& message) {
  {
    std::lock_guard<std::mutex> lock(state_.mutex);
    state_.status(message);
    update_network_summary(message);
  }
  wake_();
}

void TuiReporter::code_ready(const std::string& code, bool show_qrcode) {
  {
    std::lock_guard<std::mutex> lock(state_.mutex);
    state_.pairing_code_ready(code);
    state_.qrcode.clear();
    if (show_qrcode) {
      std::ostringstream oss;
      print_qrcode(oss, code);
      state_.qrcode = oss.str();
    }
  }
  wake_();
}

void TuiReporter::update_progress_state(UpdateKind kind, const StateMutation& mutation) {
  bool changed = false;
  bool should_wake = true;
  {
    std::lock_guard<std::mutex> lock(state_.mutex);
    changed = mutation(state_);
    if (changed && kind == UpdateKind::Progress) {
      should_wake = should_wake_progress(std::chrono::steady_clock::now());
    }
  }
  if (changed && should_wake) wake_();
}

bool TuiReporter::should_wake_progress(std::chrono::steady_clock::time_point now) {
  if (last_progress_wake_ == std::chrono::steady_clock::time_point{} ||
      now - last_progress_wake_ >= kProgressWakeInterval) {
    last_progress_wake_ = now;
    return true;
  }
  return false;
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
    state_.route_summary = text_after_prefix(message, "route result:");
  } else if (message == "direct skipped, using relay" || message == "direct failed, using relay" ||
             message == "peer selected relay; using relay path") {
    state_.route_summary = "relay (" + message + ")";
  } else if (starts_with(message, "opening ") && message.find(" parallel direct connections") != std::string::npos) {
    state_.route_summary = "direct (" + message + ")";
  } else if (starts_with(message, "parallel direct unavailable")) {
    state_.route_summary = "direct single-channel fallback";
  }
}

ftxui::Element render_transfer_view(const TuiState& state, const std::string& action_notice,
                                    bool quit_confirm_pending, const FailureRecoveryHint* recovery_hint) {
  using namespace ftxui;

  const double overall_ratio = state.overall_total > 0
                                   ? static_cast<double>(state.overall_done) / static_cast<double>(state.overall_total)
                                   : (state.finished ? 1.0 : 0.0);
  const double file_ratio = state.current_size > 0
                                ? static_cast<double>(state.current_done) / static_cast<double>(state.current_size)
                                : 0.0;

  const auto display_now = state.ended.value_or(std::chrono::steady_clock::now());
  const auto elapsed = std::chrono::duration<double>(display_now - state.started).count();
  const std::uint64_t rate = elapsed > 0.01 ? static_cast<std::uint64_t>(state.overall_done / elapsed) : 0;

  Elements left;
  left.push_back(text(state.title) | bold);
  left.push_back(separator());
  left.push_back(hbox({text("stage:    "), text(connectivity_stage(state)) | color(Color::Cyan)}));

  if (!state.code.empty()) {
    left.push_back(hbox({text("pairing code: "), text(state.code) | bold | color(Color::Yellow)}));
  }

  if (!state.outbound_summary.empty() || !state.outbound_probe_summary.empty() ||
      !state.route_plan_summary.empty() || !state.route_summary.empty() ||
      !state.route_timing.empty()) {
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
    if (!state.route_summary.empty()) {
      left.push_back(hbox({text("  path:     "), text(state.route_summary) | color(Color::GreenLight)}));
    }
    if (!state.route_timing.empty()) {
      left.push_back(hbox({text("  timing:   "), text(state.route_timing) | dim}));
    }
  }

  if (state.has_receive_plan) {
    const auto& plan = state.receive_plan;
    left.push_back(text("receive plan") | underlined);
    left.push_back(hbox({text("  incoming: "), text(std::to_string(plan.item_count) + " item(s), " +
                                                     human_bytes(plan.total_bytes))}));
    if (plan.resume_count > 0) {
      left.push_back(hbox({text("  resume:   "),
                           text(std::to_string(plan.resume_count) + " item(s), " +
                                human_bytes(plan.resume_bytes)) |
                               color(Color::GreenLight)}));
    }
    if (plan.skip_count > 0) {
      left.push_back(hbox({text("  skip:     "),
                           text(std::to_string(plan.skip_count) + " item(s), " + human_bytes(plan.skip_bytes)) |
                               color(Color::Yellow)}));
    }
    if (plan.rename_count > 0) {
      left.push_back(hbox({text("  rename:   "), text(std::to_string(plan.rename_count) + " conflict(s)")}));
    }
    if (plan.overwrite_count > 0) {
      left.push_back(hbox({text("  overwrite: "),
                           text(std::to_string(plan.overwrite_count) + " existing target(s)") |
                               color(Color::Yellow)}));
    }
    if (plan.resume_count == 0 && plan.skip_count == 0 && plan.rename_count == 0 &&
        plan.overwrite_count == 0) {
      left.push_back(text("  write all incoming items") | dim);
    }
  }

  if (!state.logs.empty()) {
    left.push_back(text("events") | underlined);
    for (const auto& line : state.logs) {
      left.push_back(text("  " + line) | dim);
    }
  }

  left.push_back(separator());
  Color activity_color = Color::GreenLight;
  if (state.failed) activity_color = Color::Red;
  if (state.canceled) activity_color = Color::Yellow;
  left.push_back(hbox({text("activity: "), text(state.activity) | color(activity_color)}));
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

  if (state.canceled) {
    left.push_back(separator());
    left.push_back(text("canceled") | color(Color::Yellow));
  }
  if (state.failed) {
    left.push_back(separator());
    left.push_back(text("error: " + state.error) | color(Color::Red));
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
  if (!action_notice.empty()) {
    left.push_back(separator());
    left.push_back(text(action_notice) | color(Color::GreenLight));
  }
  left.push_back(separator());
  if (state.finished || state.failed) {
    left.push_back(text("Tab to actions below") | dim);
  } else if (quit_confirm_pending) {
    left.push_back(text("cancel transfer? press q again, Esc to keep running") | color(Color::Yellow));
  } else {
    left.push_back(text("q: cancel transfer (confirm while transferring)") | dim);
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
