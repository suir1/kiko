#include "tui_note.hpp"

#include "core/cancellation.hpp"
#include "note/note_protocol.hpp"
#include "platform/platform.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace kiko {
namespace {

struct TuiNoteState {
  std::mutex mutex;
  std::string title = "kiko note";
  std::string code;
  std::string route;
  std::string status = "connecting";
  std::string error;
  std::vector<NoteFrame> pending_remote;
  std::uint64_t latest_local_revision = 0;
  std::uint64_t last_acked_revision = 0;
  bool connected = false;
  bool finished = false;
  bool failed = false;
  bool canceled = false;
};

struct TuiNoteRuntime {
  std::mutex mutex;
  std::condition_variable changed;
  std::unique_ptr<TcpSocket> channel;
  std::unique_ptr<StreamCipher> cipher;
  std::deque<NoteFrame> outgoing;
  std::atomic_bool done{false};
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

void close_runtime(TuiNoteRuntime& runtime) {
  std::lock_guard<std::mutex> lock(runtime.mutex);
  runtime.done.store(true);
  if (runtime.channel) runtime.channel->close();
  runtime.changed.notify_all();
}

bool runtime_connected(TuiNoteRuntime& runtime) {
  std::lock_guard<std::mutex> lock(runtime.mutex);
  return runtime.channel && runtime.cipher;
}

void queue_runtime_frame(TuiNoteRuntime& runtime, NoteFrame frame) {
  std::lock_guard<std::mutex> lock(runtime.mutex);
  if (runtime.done.load()) return;
  runtime.outgoing.push_back(std::move(frame));
  runtime.changed.notify_one();
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
  TuiNoteRuntime runtime;
  auto cancellation = std::make_shared<TransferCancellation>();
  auto run_config = config;
  run_config.cancellation = cancellation;

  auto screen = ScreenInteractive::Fullscreen();
  auto wake = [&] { screen.PostEvent(Event::Custom); };
  TuiNoteReporter reporter(state, wake);

  std::string editor_text;
  NoteDocument document;
  bool local_dirty = false;
  bool applying_remote = false;
  std::atomic_bool debounce_pending{false};
  std::optional<std::chrono::steady_clock::time_point> dirty_since;

  auto mark_local_changed = [&] {
    if (applying_remote) return;
    if (editor_text.size() > kNoteMaxBytes) {
      std::lock_guard<std::mutex> lock(state.mutex);
      state.status = "note is over 1 MiB; not synced";
      local_dirty = true;
      debounce_pending.store(true);
      dirty_since = std::chrono::steady_clock::now();
      return;
    }
    local_dirty = true;
    debounce_pending.store(true);
    dirty_since = std::chrono::steady_clock::now();
  };

  auto queue_editor_if_ready = [&] {
    if (!local_dirty || !dirty_since || !runtime_connected(runtime)) return;
    if (editor_text.size() > kNoteMaxBytes) return;
    if (std::chrono::steady_clock::now() - *dirty_since < std::chrono::milliseconds(250)) return;
    auto frame = make_note_update(document.revision + 1, editor_text);
    (void)apply_note_update(document, frame);
    const auto revision = frame.revision;
    queue_runtime_frame(runtime, std::move(frame));
    local_dirty = false;
    debounce_pending.store(false);
    dirty_since.reset();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.latest_local_revision = std::max(state.latest_local_revision, revision);
    state.status = "syncing";
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
      if (apply_note_update(document, frame)) {
        applying_remote = true;
        editor_text = document.text;
        applying_remote = false;
        local_dirty = false;
        debounce_pending.store(false);
        dirty_since.reset();
        queue_runtime_frame(runtime, make_note_ack(frame.revision));
      }
    }
  };

  std::thread sender([&] {
    try {
      while (true) {
        NoteFrame frame;
        TcpSocket* channel = nullptr;
        StreamCipher* cipher = nullptr;
        {
          std::unique_lock<std::mutex> lock(runtime.mutex);
          runtime.changed.wait(lock, [&] {
            return runtime.done.load() ||
                   (runtime.channel && runtime.cipher && !runtime.outgoing.empty());
          });
          if (runtime.done.load() && runtime.outgoing.empty()) break;
          if (!runtime.channel || !runtime.cipher || runtime.outgoing.empty()) continue;
          frame = std::move(runtime.outgoing.front());
          runtime.outgoing.pop_front();
          channel = runtime.channel.get();
          cipher = runtime.cipher.get();
        }
        send_note_frame(*channel, *cipher, frame);
        if (frame.type == NoteFrameType::Update || frame.type == NoteFrameType::Clear) {
          std::lock_guard<std::mutex> lock(state.mutex);
          state.latest_local_revision = std::max(state.latest_local_revision, frame.revision);
          state.status = state.last_acked_revision >= state.latest_local_revision ? "synced" : "sent";
        }
        wake();
      }
    } catch (const std::exception& error) {
      if (!runtime.done.load()) set_failed(state, error.what());
      close_runtime(runtime);
      wake();
    }
  });

  std::thread worker([&] {
    try {
      auto session = open_peer_session(run_config, reporter);
      auto channel = std::make_unique<TcpSocket>(std::move(session.encrypted.channel));
      auto cipher = std::make_unique<StreamCipher>(session.encrypted.key, run_config.role == Role::Sender);
      send_note_frame(*channel, *cipher, make_note_hello());
      auto hello = recv_note_frame(*channel, *cipher);
      if (!hello || hello->type != NoteFrameType::Hello) throw KikoError("note peer did not send hello");

      {
        std::lock_guard<std::mutex> lock(runtime.mutex);
        runtime.channel = std::move(channel);
        runtime.cipher = std::move(cipher);
      }
      runtime.changed.notify_all();
      {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.connected = true;
        state.route = session.outcome.data_path;
        state.status = "connected";
      }
      wake();

      while (!runtime.done.load()) {
        NoteFrame frame;
        TcpSocket* active_channel = nullptr;
        StreamCipher* active_cipher = nullptr;
        {
          std::lock_guard<std::mutex> lock(runtime.mutex);
          active_channel = runtime.channel.get();
          active_cipher = runtime.cipher.get();
        }
        if (!active_channel || !active_cipher) break;
        auto incoming = recv_note_frame(*active_channel, *active_cipher);
        if (!incoming) break;
        frame = std::move(*incoming);
        if (frame.type == NoteFrameType::Bye) break;
        if (frame.type == NoteFrameType::Ack) {
          {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.last_acked_revision = std::max(state.last_acked_revision, frame.revision);
            state.status = state.last_acked_revision >= state.latest_local_revision ? "synced" : "sent";
          }
          wake();
          continue;
        }
        if (frame.type != NoteFrameType::Update && frame.type != NoteFrameType::Clear) continue;
        {
          std::lock_guard<std::mutex> lock(state.mutex);
          state.pending_remote.push_back(std::move(frame));
          state.status = "remote update";
        }
        wake();
      }
      {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (!state.failed && !state.canceled) {
          state.finished = true;
          state.status = "peer closed";
        }
      }
    } catch (const std::exception& error) {
      if (cancellation->requested() || runtime.done.load()) {
        set_canceled(state);
      } else {
        set_failed(state, error.what());
      }
    }
    close_runtime(runtime);
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
    auto frame = make_note_clear(document.revision + 1);
    (void)apply_note_update(document, frame);
    const auto revision = frame.revision;
    queue_runtime_frame(runtime, std::move(frame));
    local_dirty = false;
    debounce_pending.store(false);
    dirty_since.reset();
    {
      std::lock_guard<std::mutex> lock(state.mutex);
      state.latest_local_revision = std::max(state.latest_local_revision, revision);
      state.status = "syncing";
    }
    wake();
  });
  auto quit_button = Button("Quit", [&] {
    cancellation->request();
    close_runtime(runtime);
    screen.Exit();
  });
  auto controls = Container::Horizontal({clear_button, quit_button});
  auto layout = Container::Vertical({editor, controls});

  auto renderer = Renderer(layout, [&] {
    apply_remote_updates();
    queue_editor_if_ready();

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
    header.push_back(text("status: " + status));
    if (failed) header.push_back(text("error: " + error) | color(Color::Red));
    if (canceled) header.push_back(text("canceled") | color(Color::Yellow));

    return vbox({
               vbox(std::move(header)),
               separator(),
               editor->Render() | vscroll_indicator | frame | flex,
               separator(),
               hbox({clear_button->Render(), text(" "), quit_button->Render(), filler(),
                     text("Esc to cancel") | dim}),
           }) |
           border;
  });

  auto with_events = CatchEvent(renderer, [&](Event event) {
    if (event == Event::Custom) {
      apply_remote_updates();
      queue_editor_if_ready();
      return false;
    }
    if (event == Event::Escape) {
      cancellation->request();
      close_runtime(runtime);
      screen.Exit();
      return true;
    }
    return false;
  });

  screen.Loop(with_events);
  ticker_done.store(true);
  close_runtime(runtime);
  if (ticker.joinable()) ticker.join();
  if (worker.joinable()) worker.join();
  if (sender.joinable()) sender.join();

  std::lock_guard<std::mutex> lock(state.mutex);
  if (state.failed) {
    std::cerr << "error: " << state.error << "\n";
    return 1;
  }
  if (state.canceled) return 130;
  return 0;
}

}  // namespace kiko
