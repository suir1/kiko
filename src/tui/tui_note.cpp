#include "tui_note.hpp"

#include "core/qrcode_print.hpp"
#include "note/note_session.hpp"
#include "note/note_workspace.hpp"
#include "platform/platform.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace kiko {
namespace {

constexpr std::size_t kNoteQrMaxBytes = 1200;

struct TuiNoteState {
  std::mutex mutex;
  std::string title = "kiko note";
  std::string code;
  std::string route;
  std::string status = "connecting";
  std::string error;
  std::vector<NoteFrame> pending_remote;
  bool finished = false;
  bool failed = false;
  bool canceled = false;
};

class TuiNoteReporter : public ProgressReporter {
 public:
  TuiNoteReporter(TuiNoteState& state, std::function<void()> wake)
      : state_(state), wake_(std::move(wake)) {}

  void status(const std::string& message) override {
    {
      std::lock_guard<std::mutex> lock(state_.mutex);
      state_.status = message;
    }
    wake_();
  }

  void code_ready(const std::string& code, bool show_qrcode) override {
    (void)show_qrcode;
    {
      std::lock_guard<std::mutex> lock(state_.mutex);
      state_.code = code;
      state_.status = "waiting for peer";
    }
    wake_();
  }

  void route_phase(RoutePhase phase, const RoutePhaseDetail& detail) override {
    (void)phase;
    {
      std::lock_guard<std::mutex> lock(state_.mutex);
      state_.status = detail.message.empty() ? "connecting" : detail.message;
    }
    wake_();
  }

  void route_outcome(const RouteOutcome& outcome) override {
    {
      std::lock_guard<std::mutex> lock(state_.mutex);
      state_.route = outcome.data_path;
    }
    wake_();
  }

  void handshake_ok() override {
    {
      std::lock_guard<std::mutex> lock(state_.mutex);
      state_.status = "secure channel ready";
    }
    wake_();
  }

 private:
  TuiNoteState& state_;
  std::function<void()> wake_;
};

void set_failed(TuiNoteState& state, const std::string& error) {
  std::lock_guard<std::mutex> lock(state.mutex);
  if (state.failed) return;
  state.failed = true;
  state.finished = true;
  state.error = error;
  state.status = "error";
}

void set_canceled(TuiNoteState& state) {
  std::lock_guard<std::mutex> lock(state.mutex);
  if (state.failed) return;
  state.canceled = true;
  state.finished = true;
  state.status = "canceled";
}

void set_status(TuiNoteState& state, std::string status) {
  std::lock_guard<std::mutex> lock(state.mutex);
  state.status = std::move(status);
}

std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream input(text);
  std::string line;
  while (std::getline(input, line)) lines.push_back(line);
  if (lines.empty()) lines.push_back("");
  return lines;
}

std::string note_pad_title(const NoteDocument& document) {
  if (!document.title.empty()) return document.title;
  return document.pad_id.empty() ? "main" : document.pad_id;
}

std::size_t active_pad_index(const NoteWorkspaceSnapshot& snapshot) {
  const auto it = std::find_if(snapshot.documents.begin(), snapshot.documents.end(), [&](const NoteDocument& document) {
    return document.pad_id == snapshot.active_pad;
  });
  return it == snapshot.documents.end() ? 0 : static_cast<std::size_t>(std::distance(snapshot.documents.begin(), it));
}

}  // namespace

int run_tui_note(const NoteConfig& config) {
  if (!stdin_is_tty()) {
    CliReporter reporter;
    return run_note(config, reporter);
  }

  using namespace ftxui;

  TuiNoteState state;
  state.title = config.role == Role::Sender ? "kiko note host" : "kiko note join";
  NoteWorkspace workspace;

  auto run_config = config;
  auto screen = ScreenInteractive::Fullscreen();
  auto wake = [&] { screen.PostEvent(Event::Custom); };
  TuiNoteReporter reporter(state, wake);

  NoteSessionCallbacks callbacks;
  callbacks.connected = [&](const NoteSessionInfo& info) {
    {
      std::lock_guard<std::mutex> lock(state.mutex);
      state.code = info.code;
      state.route = info.outcome.data_path;
      state.status = "connected";
    }
    wake();
  };
  callbacks.frame_received = [&](const NoteFrame& frame) {
    if (frame.type == NoteFrameType::Ack) {
      workspace.acknowledge(frame);
      set_status(state, workspace.snapshot().synced ? "synced" : "sent");
      wake();
      return;
    }
    if (frame.type != NoteFrameType::Update && frame.type != NoteFrameType::Clear) return;
    {
      std::lock_guard<std::mutex> lock(state.mutex);
      state.pending_remote.push_back(frame);
      state.status = "remote update";
    }
    wake();
  };
  callbacks.frame_sent = [&](const NoteFrame& frame) {
    if (frame.type != NoteFrameType::Update && frame.type != NoteFrameType::Clear) return;
    set_status(state, workspace.snapshot().synced ? "synced" : "sent");
    wake();
  };

  auto session = std::make_shared<NoteSession>(run_config, reporter, std::move(callbacks));
  auto cancellation = session->cancellation();

  std::string editor_text;
  bool local_dirty = false;
  bool applying_remote = false;
  std::string note_qr;
  std::atomic_bool debounce_pending{false};
  std::optional<std::chrono::steady_clock::time_point> dirty_since;

  auto mark_local_changed = [&] {
    if (applying_remote) return;
    note_qr.clear();
    local_dirty = true;
    debounce_pending.store(true);
    dirty_since = std::chrono::steady_clock::now();
    if (editor_text.size() > kNoteMaxBytes) set_status(state, "note is over 1 MiB; not synced");
  };

  auto send_editor = [&](bool wait_for_debounce) {
    if (!local_dirty || !dirty_since || editor_text.size() > kNoteMaxBytes) return;
    if (wait_for_debounce &&
        std::chrono::steady_clock::now() - *dirty_since < std::chrono::milliseconds(250)) {
      return;
    }
    auto frame = workspace.update_active(editor_text);
    if (!session->send(std::move(frame))) return;
    local_dirty = false;
    debounce_pending.store(false);
    dirty_since.reset();
    set_status(state, session->connected() ? "syncing" : "edited locally");
  };

  auto switch_active_pad = [&](const std::string& pad_id) {
    if (pad_id.empty() || pad_id == workspace.snapshot().active_pad) return;
    send_editor(false);
    if (!workspace.select_pad(pad_id)) return;
    applying_remote = true;
    editor_text = workspace.active_document().text;
    note_qr.clear();
    applying_remote = false;
    local_dirty = false;
    debounce_pending.store(false);
    dirty_since.reset();
  };

  auto apply_remote_updates = [&] {
    if (local_dirty) {
      std::lock_guard<std::mutex> lock(state.mutex);
      if (!state.pending_remote.empty()) state.status = "remote update pending";
      return;
    }

    std::vector<NoteFrame> frames;
    {
      std::lock_guard<std::mutex> lock(state.mutex);
      frames.swap(state.pending_remote);
    }
    for (const auto& frame : frames) {
      if (!workspace.apply_remote(frame)) continue;
      if (workspace.snapshot().active_pad != frame.pad_id) continue;
      applying_remote = true;
      editor_text = workspace.active_document().text;
      note_qr.clear();
      applying_remote = false;
      local_dirty = false;
      debounce_pending.store(false);
      dirty_since.reset();
    }
  };

  std::thread worker([&] {
    try {
      const auto result = session->run();
      if (result == NoteSessionEnd::Stopped || cancellation->requested()) {
        set_canceled(state);
      } else {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (!state.failed && !state.canceled) {
          state.finished = true;
          state.status = "peer closed";
        }
      }
    } catch (const std::exception& error) {
      if (cancellation->requested()) {
        set_canceled(state);
      } else {
        set_failed(state, error.what());
      }
    }
    wake();
  });

  std::atomic_bool ticker_done{false};
  std::thread ticker([&] {
    while (!ticker_done.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      if (!ticker_done.load() && debounce_pending.load()) wake();
    }
  });

  InputOption input_options;
  input_options.multiline = true;
  input_options.on_change = mark_local_changed;
  auto editor = Input(&editor_text, "shared note", input_options);
  auto clear_button = Button("Clear", [&] {
    editor_text.clear();
    note_qr.clear();
    if (session->send(workspace.clear_active())) set_status(state, "syncing");
    local_dirty = false;
    debounce_pending.store(false);
    dirty_since.reset();
    wake();
  });
  auto prev_button = Button("Prev", [&] {
    const auto snapshot = workspace.snapshot();
    if (snapshot.documents.size() <= 1) return;
    const auto index = active_pad_index(snapshot);
    switch_active_pad(snapshot.documents[(index + snapshot.documents.size() - 1) % snapshot.documents.size()].pad_id);
    wake();
  });
  auto next_button = Button("Next", [&] {
    const auto snapshot = workspace.snapshot();
    if (snapshot.documents.size() <= 1) return;
    const auto index = active_pad_index(snapshot);
    switch_active_pad(snapshot.documents[(index + 1) % snapshot.documents.size()].pad_id);
    wake();
  });
  auto new_button = Button("New", [&] {
    send_editor(false);
    auto frame = workspace.create_pad();
    const auto title = frame.title;
    (void)session->send(std::move(frame));
    applying_remote = true;
    editor_text = workspace.active_document().text;
    note_qr.clear();
    applying_remote = false;
    local_dirty = false;
    debounce_pending.store(false);
    dirty_since.reset();
    set_status(state, "created " + title);
    wake();
  });
  auto qr_button = Button("QR", [&] {
    if (!note_qr.empty()) {
      note_qr.clear();
      wake();
      return;
    }
    if (editor_text.empty()) {
      set_status(state, "note is empty; QR unavailable");
      wake();
      return;
    }
    if (editor_text.size() > kNoteQrMaxBytes) {
      set_status(state, "note QR supports up to 1200 bytes");
      wake();
      return;
    }
    try {
      std::ostringstream out;
      print_qrcode(out, editor_text);
      note_qr = out.str();
      set_status(state, "QR encodes note text directly");
    } catch (...) {
      set_status(state, "QR unavailable for this note");
    }
    wake();
  });
  auto quit_button = Button("Quit", [&] {
    session->request_stop();
    screen.Exit();
  });
  auto controls = Container::Horizontal({prev_button, next_button, new_button, clear_button, qr_button, quit_button});
  auto layout = Container::Vertical({editor, controls});

  auto renderer = Renderer(layout, [&] {
    apply_remote_updates();
    send_editor(true);

    std::string title;
    std::string code;
    std::string route;
    std::string status;
    std::string error;
    bool failed = false;
    bool canceled = false;
    {
      std::lock_guard<std::mutex> lock(state.mutex);
      title = state.title;
      code = state.code;
      route = state.route;
      status = state.status;
      error = state.error;
      failed = state.failed;
      canceled = state.canceled;
    }

    Elements header;
    header.push_back(text(title) | bold);
    if (!code.empty()) header.push_back(text("code: " + code));
    if (!route.empty()) header.push_back(text("route: " + route));
    const auto snapshot = workspace.snapshot();
    const auto document = workspace.active_document();
    header.push_back(text("pad: " + note_pad_title(document) + " (" +
                          std::to_string(active_pad_index(snapshot) + 1) + "/" +
                          std::to_string(std::max<std::size_t>(snapshot.documents.size(), 1)) + ")"));
    header.push_back(text("status: " + status));
    if (failed) header.push_back(text("error: " + error) | color(Color::Red));
    if (canceled) header.push_back(text("canceled") | color(Color::Yellow));

    Element editor_panel = editor->Render() | vscroll_indicator | frame | flex;
    if (!note_qr.empty()) {
      Elements qr_rows;
      for (const auto& line : split_lines(note_qr)) qr_rows.push_back(text(line));
      editor_panel = hbox({
                         editor_panel,
                         separator(),
                         vbox({
                             text("Note QR") | bold,
                             text("direct text") | dim,
                             separator(),
                             vbox(std::move(qr_rows)),
                         }) | border,
                     }) |
                     flex;
    }

    return vbox({
               vbox(std::move(header)),
               separator(),
               editor_panel,
               separator(),
               hbox({prev_button->Render(), text(" "), next_button->Render(), text(" "),
                     new_button->Render(), text(" "), clear_button->Render(), text(" "),
                     qr_button->Render(), text(" "), quit_button->Render(), filler(),
                     text("Esc to cancel") | dim}),
           }) |
           border;
  });

  auto with_events = CatchEvent(renderer, [&](Event event) {
    if (event == Event::Custom) {
      apply_remote_updates();
      send_editor(true);
      return false;
    }
    if (event == Event::Escape) {
      session->request_stop();
      screen.Exit();
      return true;
    }
    return false;
  });

  screen.Loop(with_events);
  ticker_done.store(true);
  session->request_stop();
  if (ticker.joinable()) ticker.join();
  if (worker.joinable()) worker.join();

  std::lock_guard<std::mutex> lock(state.mutex);
  if (state.failed) {
    std::cerr << "error: " << state.error << "\n";
    return 1;
  }
  if (state.canceled) return 130;
  return 0;
}

}  // namespace kiko
