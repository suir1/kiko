#include "tui.hpp"

#include "doctor.hpp"
#include "platform.hpp"
#include "progress.hpp"
#include "qrcode_print.hpp"
#include "tui_browser.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace kiko {
namespace {

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

std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) lines.push_back(line);
  return lines;
}

std::optional<std::string> relay_pass_from_env() {
  if (const char* env = std::getenv("KIKO_RELAY_PASS")) {
    if (env[0] != '\0') return std::string(env);
  }
  return std::nullopt;
}

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
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

class TuiReporter : public ProgressReporter {
 public:
  TuiReporter(TuiState& state, std::function<void()> wake) : state_(state), wake_(std::move(wake)) {}

  void status(const std::string& message) override {
    {
      std::lock_guard<std::mutex> lock(state_.mutex);
      log_append(state_.connectivity_log, message);
      update_network_summary(message);
      state_.activity = message;
    }
    wake_();
  }

  void connectivity_report(const std::string& report) override {
    {
      std::lock_guard<std::mutex> lock(state_.mutex);
      log_append(state_.connectivity_log, report);
      state_.activity = "connectivity probe finished";
    }
    wake_();
  }

  void handshake_ok() override {
    {
      std::lock_guard<std::mutex> lock(state_.mutex);
      state_.handshake = true;
      state_.activity = "encrypted channel ready";
      log_append(state_.connectivity_log, "handshake ok (PAKE + XChaCha20-Poly1305)");
    }
    wake_();
  }

  void code_ready(const std::string& code, bool show_qrcode = true) override {
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

  void transfer_overview(std::size_t file_count, std::uint64_t total_bytes) override {
    {
      std::lock_guard<std::mutex> lock(state_.mutex);
      state_.files_total = file_count;
      state_.overall_total = total_bytes;
      state_.activity = "transferring " + std::to_string(file_count) + " file(s)";
    }
    wake_();
  }

  void file_start(const std::string& path, std::uint64_t size) override {
    {
      std::lock_guard<std::mutex> lock(state_.mutex);
      state_.current_file = path;
      state_.current_size = size;
      state_.current_done = 0;
    }
    wake_();
  }

  void file_advance(std::uint64_t bytes_delta) override {
    {
      std::lock_guard<std::mutex> lock(state_.mutex);
      state_.current_done += bytes_delta;
      state_.overall_done += bytes_delta;
    }
    wake_();
  }

  void file_complete(const std::string& path, std::uint64_t size, bool verified) override {
    {
      std::lock_guard<std::mutex> lock(state_.mutex);
      (void)path;
      (void)size;
      (void)verified;
      ++state_.files_done;
    }
    wake_();
  }

  void transfer_complete(std::size_t file_count, std::uint64_t total_bytes) override {
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

 private:
  void update_network_summary(const std::string& message) {
    if (starts_with(message, "outbound probe:")) {
      state_.outbound_probe_summary = text_after_prefix(message, "outbound probe:");
    } else if (starts_with(message, "outbound interface:")) {
      state_.outbound_summary = text_after_prefix(message, "outbound interface:");
    } else if (starts_with(message, "outbound path:")) {
      state_.outbound_summary = text_after_prefix(message, "outbound path:");
    } else if (starts_with(message, "route plan:")) {
      state_.route_plan_summary = text_after_prefix(message, "route plan:");
    } else if (message == "direct skipped, using relay" || message == "direct failed, using relay" ||
               message == "peer selected relay; using relay path") {
      state_.transfer_path_summary = "relay (" + message + ")";
    } else if (starts_with(message, "opening ") && message.find(" parallel direct connections") != std::string::npos) {
      state_.transfer_path_summary = "direct (" + message + ")";
    } else if (starts_with(message, "parallel direct unavailable")) {
      state_.transfer_path_summary = "direct single-channel fallback";
    }
  }

  TuiState& state_;
  std::function<void()> wake_;
};

ftxui::Element render_transfer_view(const TuiState& state) {
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

  if (state.failed) {
    left.push_back(separator());
    left.push_back(text("error: " + state.error_message) | color(Color::Red));
  }
  left.push_back(separator());
  left.push_back(text(state.finished ? "done — press q to quit" : "waiting — press q to quit") | dim);

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

int run_transfer_screen(const std::string& title, const std::function<void(ProgressReporter&)>& run) {
  using namespace ftxui;

  TuiState state;
  state.title = title;

  auto screen = ScreenInteractive::Fullscreen();
  TuiReporter reporter(state, [&] { screen.PostEvent(Event::Custom); });

  std::thread worker([&] {
    try {
      run(reporter);
    } catch (const std::exception& e) {
      std::lock_guard<std::mutex> lock(state.mutex);
      state.failed = true;
      state.finished = true;
      state.error_message = e.what();
      state.activity = "error";
    }
    screen.PostEvent(Event::Custom);
  });

  auto renderer = Renderer([&] {
    std::lock_guard<std::mutex> lock(state.mutex);
    return render_transfer_view(state);
  });

  auto with_events = CatchEvent(renderer, [&](Event event) {
    if (event == Event::Character('q') || event == Event::Escape) {
      screen.Exit();
      return true;
    }
    return false;
  });

  screen.Loop(with_events);
  worker.join();

  std::lock_guard<std::mutex> lock(state.mutex);
  if (state.failed) {
    std::cerr << "error: " << state.error_message << "\n";
    return 1;
  }
  return 0;
}

void show_doctor_modal(const Endpoint& relay, const std::optional<std::string>& relay_pass) {
  using namespace ftxui;

  DoctorOptions opts;
  opts.relay = relay;
  opts.relay_pass = relay_pass;

  std::mutex mutex;
  std::string status = "running network check...";
  std::string body;
  bool failed = false;

  auto screen = ScreenInteractive::Fullscreen();
  std::thread worker([&] {
    try {
      const DoctorReport report = run_doctor(opts);
      std::lock_guard<std::mutex> lock(mutex);
      body = report.diagnosis;
      if (body.empty()) body = "check finished (no issues reported)";
      status = "done";
    } catch (const std::exception& e) {
      std::lock_guard<std::mutex> lock(mutex);
      failed = true;
      body = e.what();
      status = "failed";
    }
    screen.PostEvent(Event::Custom);
  });

  bool closed = false;
  auto close_button = Button("Close", [&] {
    closed = true;
    screen.Exit();
  });

  auto renderer = Renderer(close_button, [&] {
    std::lock_guard<std::mutex> lock(mutex);
    Elements rows;
    rows.push_back(text("kiko network check") | bold | hcenter);
    rows.push_back(hbox({text("relay: "), text(relay.to_string())}));
    rows.push_back(hbox({text("status: "), text(status) | color(failed ? Color::Red : Color::GreenLight)}));
    rows.push_back(separator());
    for (const auto& line : split_lines(body.empty() ? status : body)) {
      rows.push_back(text(line));
    }
    rows.push_back(separator());
    rows.push_back(close_button->Render() | hcenter);
    return vbox(std::move(rows)) | border;
  });

  auto with_events = CatchEvent(renderer, [&](Event event) {
    if (event == Event::Escape) {
      closed = true;
      screen.Exit();
      return true;
    }
    return false;
  });

  screen.Loop(with_events);
  worker.join();
  (void)closed;
}

std::optional<std::string> resolve_relay_pass(const std::string& input) {
  if (!input.empty()) return input;
  return relay_pass_from_env();
}

}  // namespace

int run_tui_send(const SendConfig& config) {
  if (!stdin_is_tty()) {
    CliReporter reporter;
    return run_send(config, reporter);
  }
  return run_transfer_screen("kiko send", [&](ProgressReporter& reporter) { run_send(config, reporter); });
}

int run_tui_recv(const RecvConfig& config) {
  if (!stdin_is_tty()) {
    CliReporter reporter;
    return run_recv(config, reporter);
  }
  return run_transfer_screen("kiko receive", [&](ProgressReporter& reporter) { run_recv(config, reporter); });
}

int run_tui_menu(const Endpoint& default_relay) {
  using namespace ftxui;

  if (!stdin_is_tty()) {
    std::cerr << "kiko tui requires an interactive terminal\n";
    return 2;
  }

  int mode = 0;
  std::vector<std::string> modes = {"Send", "Receive"};

  std::string relay = default_relay.to_string();
  std::string relay_pass;
  if (auto env_pass = relay_pass_from_env()) relay_pass = *env_pass;

  std::string path;
  std::string code;
  std::string out_dir = ".";
  std::string menu_error;

  int screen_tab = 0;  // 0 = menu, 1 = transfer
  TuiState transfer_state;
  std::thread worker;
  bool worker_started = false;

  auto screen = ScreenInteractive::Fullscreen();
  auto wake = [&] { screen.PostEvent(Event::Custom); };

  auto mode_toggle = Toggle(&modes, &mode);

  InputOption relay_opt;
  relay_opt.multiline = false;
  auto relay_input = Input(&relay, "host:port", relay_opt);

  InputOption pass_opt;
  pass_opt.password = true;
  pass_opt.multiline = false;
  auto relay_pass_input = Input(&relay_pass, "relay password (optional)", pass_opt);

  auto path_input = Input(&path, "file or directory to send");
  auto code_input = Input(&code, "pairing code");
  auto out_input = Input(&out_dir, "output directory");

  auto path_browse = Button("Browse…", [&] {
    const auto start_path = path.empty() ? std::filesystem::current_path() : std::filesystem::path(path);
    if (auto picked = run_tui_path_picker(start_path, TuiPickMode::FileOrDirectory)) {
      path = picked->string();
      menu_error.clear();
    }
  });

  auto out_browse = Button("Browse…", [&] {
    const auto start_path = out_dir.empty() ? std::filesystem::current_path() : std::filesystem::path(out_dir);
    if (auto picked = run_tui_path_picker(start_path, TuiPickMode::DirectoryOnly)) {
      out_dir = picked->string();
      menu_error.clear();
    }
  });

  auto doctor_button = Button("Network check", [&] {
    Endpoint relay_ep;
    try {
      relay_ep = parse_endpoint(relay);
    } catch (const std::exception& e) {
      menu_error = std::string("invalid relay: ") + e.what();
      return;
    }
    show_doctor_modal(relay_ep, resolve_relay_pass(relay_pass));
  });

  auto begin_transfer = [&]() -> bool {
    menu_error.clear();
    if (mode == 0 && path.empty()) {
      menu_error = "path is required — type a path or press Browse";
      return false;
    }
    if (mode == 1 && code.empty()) {
      menu_error = "pairing code is required";
      return false;
    }

    Endpoint relay_ep;
    try {
      relay_ep = parse_endpoint(relay);
    } catch (const std::exception& e) {
      menu_error = std::string("invalid relay: ") + e.what();
      return false;
    }

    if (mode == 0) {
      std::error_code ec;
      if (!std::filesystem::exists(path, ec)) {
        menu_error = "path does not exist: " + path;
        return false;
      }
    }

    const auto relay_pass_opt = resolve_relay_pass(relay_pass);

    transfer_state.title = mode == 0 ? "kiko send" : "kiko receive";
    transfer_state.code.clear();
    transfer_state.qrcode.clear();
    transfer_state.connectivity_log.clear();
    transfer_state.outbound_summary.clear();
    transfer_state.outbound_probe_summary.clear();
    transfer_state.route_plan_summary.clear();
    transfer_state.transfer_path_summary.clear();
    transfer_state.activity = "starting...";
    transfer_state.current_file.clear();
    transfer_state.current_done = 0;
    transfer_state.current_size = 0;
    transfer_state.overall_done = 0;
    transfer_state.overall_total = 0;
    transfer_state.files_total = 0;
    transfer_state.files_done = 0;
    transfer_state.handshake = false;
    transfer_state.finished = false;
    transfer_state.failed = false;
    transfer_state.error_message.clear();
    transfer_state.start = std::chrono::steady_clock::now();

    const int mode_copy = mode;
    const std::string path_copy = path;
    const std::string code_copy = code;
    const std::string out_copy = out_dir;

    worker = std::thread([&, relay_ep, relay_pass_opt, mode_copy, path_copy, code_copy, out_copy]() {
      TuiReporter reporter(transfer_state, wake);
      try {
        if (mode_copy == 0) {
          SendConfig config;
          config.file = path_copy;
          config.relay = relay_ep;
          config.code = code_copy;
          config.relay_pass = relay_pass_opt;
          config.show_qrcode = true;
          run_send(config, reporter);
        } else {
          RecvConfig config;
          config.code = code_copy;
          config.relay = relay_ep;
          config.output_dir = out_copy;
          config.relay_pass = relay_pass_opt;
          run_recv(config, reporter);
        }
      } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(transfer_state.mutex);
        transfer_state.failed = true;
        transfer_state.finished = true;
        transfer_state.error_message = e.what();
        transfer_state.activity = "error";
      }
      wake();
    });
    worker_started = true;
    screen_tab = 1;
    return true;
  };

  auto start_button = Button("Start", [&] {
    begin_transfer();
    wake();
  });

  auto menu_layout = Container::Vertical({
      mode_toggle,
      relay_input,
      relay_pass_input,
      path_input,
      path_browse,
      code_input,
      out_input,
      out_browse,
      doctor_button,
      start_button,
  });

  auto menu_renderer = Renderer(menu_layout, [&] {
    Elements rows;
    rows.push_back(text("kiko") | bold | hcenter);
    rows.push_back(separator());
    rows.push_back(hbox({text("mode:  "), mode_toggle->Render()}));
    rows.push_back(hbox({text("relay: "), relay_input->Render() | flex}));
    rows.push_back(hbox({text("pass:  "), relay_pass_input->Render() | flex}));
    if (mode == 0) {
      rows.push_back(hbox({text("path:  "), path_input->Render() | flex, path_browse->Render()}));
      rows.push_back(hbox({text("code:  "), code_input->Render() | flex, text(" (optional)") | dim}));
    } else {
      rows.push_back(hbox({text("code:  "), code_input->Render() | flex}));
      rows.push_back(hbox({text("out:   "), out_input->Render() | flex, out_browse->Render()}));
    }
    rows.push_back(separator());
    rows.push_back(hbox({doctor_button->Render(), text("  "), start_button->Render()}) | hcenter);
    if (!menu_error.empty()) {
      rows.push_back(text(menu_error) | color(Color::Red));
    }
    rows.push_back(text("Tab to move, Enter on Start; q/Esc to quit") | dim);
    if (relay_pass_from_env() && relay_pass.empty()) {
      rows.push_back(text("hint: KIKO_RELAY_PASS is set and will be used") | dim);
    }
    return vbox(std::move(rows)) | border;
  });

  auto transfer_renderer = Renderer([&] {
    std::lock_guard<std::mutex> lock(transfer_state.mutex);
    return render_transfer_view(transfer_state);
  });

  auto tabs = Container::Tab({menu_renderer, transfer_renderer}, &screen_tab);

  auto with_events = CatchEvent(tabs, [&](Event event) {
    if (event == Event::Character('q') || event == Event::Escape) {
      screen.Exit();
      return true;
    }
    return false;
  });

  screen.Loop(with_events);

  if (worker_started && worker.joinable()) worker.join();

  std::lock_guard<std::mutex> lock(transfer_state.mutex);
  if (transfer_state.failed) {
    std::cerr << "error: " << transfer_state.error_message << "\n";
    return 1;
  }
  return 0;
}

}  // namespace kiko
