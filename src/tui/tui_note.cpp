#include "tui_note.hpp"

#include "core/cancellation.hpp"
#include "core/qrcode_print.hpp"
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
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace kiko {
namespace {

constexpr auto kNoteReadPoll = std::chrono::milliseconds(100);
constexpr auto kNoteHelloTimeout = std::chrono::seconds(20);
constexpr std::size_t kNoteQrMaxBytes = 1200;

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

std::optional<NoteFrame> recv_tui_note_interruptible(TcpSocket& channel, StreamCipher& cipher,
                                                     const TuiNoteRuntime& runtime,
                                                     const TransferCancellation& cancellation) {
  while (!runtime.done.load() && !cancellation.requested()) {
    auto frame = recv_note_frame_timeout(channel, cipher, kNoteReadPoll, cancellation.flag());
    if (frame) return frame;
    if (!channel.valid()) return std::nullopt;
  }
  return std::nullopt;
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

std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream input(text);
  std::string line;
  while (std::getline(input, line)) {
    lines.push_back(line);
  }
  if (lines.empty()) lines.push_back("");
  return lines;
}

NoteDocument& ensure_tui_note_pad(std::map<std::string, NoteDocument>& documents, const std::string& pad_id,
                                  const std::string& title = {}) {
  const auto id = pad_id.empty() ? std::string("main") : pad_id;
  auto [it, inserted] = documents.try_emplace(id);
  if (inserted) {
    it->second.pad_id = id;
    it->second.title = title.empty() ? (id == "main" ? "Note 1" : id) : title;
  } else if (!title.empty()) {
    it->second.title = title;
  }
  return it->second;
}

std::vector<std::string> sorted_tui_note_pads(const std::map<std::string, NoteDocument>& documents) {
  std::vector<std::string> ids;
  ids.reserve(documents.size());
  for (const auto& [id, _] : documents) ids.push_back(id);
  std::sort(ids.begin(), ids.end(), [](const std::string& a, const std::string& b) {
    if (a == "main") return true;
    if (b == "main") return false;
    return a < b;
  });
  return ids;
}

std::string note_pad_title(const NoteDocument& document) {
  if (!document.title.empty()) return document.title;
  return document.pad_id.empty() ? "main" : document.pad_id;
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
  std::map<std::string, NoteDocument> documents;
  std::string active_pad = "main";
  int next_pad_number = 2;
  (void)ensure_tui_note_pad(documents, active_pad, "Note 1");
  bool local_dirty = false;
  bool applying_remote = false;
  std::string note_qr;
  std::atomic_bool debounce_pending{false};
  std::optional<std::chrono::steady_clock::time_point> dirty_since;

  auto mark_local_changed = [&] {
    if (applying_remote) return;
    note_qr.clear();
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
    auto& document = ensure_tui_note_pad(documents, active_pad);
    auto frame = make_note_update(document.pad_id, document.revision + 1, editor_text, document.title);
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

  auto flush_active_editor = [&] {
    if (!local_dirty || editor_text.size() > kNoteMaxBytes) return;
    auto& document = ensure_tui_note_pad(documents, active_pad);
    auto frame = make_note_update(document.pad_id, document.revision + 1, editor_text, document.title);
    (void)apply_note_update(document, frame);
    const auto revision = frame.revision;
    if (runtime_connected(runtime)) queue_runtime_frame(runtime, std::move(frame));
    local_dirty = false;
    debounce_pending.store(false);
    dirty_since.reset();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.latest_local_revision = std::max(state.latest_local_revision, revision);
    state.status = runtime_connected(runtime) ? "syncing" : "edited locally";
  };

  auto switch_active_pad = [&](const std::string& pad_id) {
    if (pad_id.empty() || pad_id == active_pad) return;
    flush_active_editor();
    active_pad = pad_id;
    auto& document = ensure_tui_note_pad(documents, active_pad);
    applying_remote = true;
    editor_text = document.text;
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
      auto& document = ensure_tui_note_pad(documents, frame.pad_id, frame.title);
      if (apply_note_update(document, frame)) {
        if (document.pad_id == active_pad) {
          applying_remote = true;
          editor_text = document.text;
          note_qr.clear();
          applying_remote = false;
          local_dirty = false;
          debounce_pending.store(false);
          dirty_since.reset();
        }
        queue_runtime_frame(runtime, make_note_ack(document.pad_id, frame.revision));
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
      auto hello = recv_note_frame_timeout(*channel, *cipher, kNoteHelloTimeout, cancellation->flag());
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
        auto incoming = recv_tui_note_interruptible(*active_channel, *active_cipher, runtime, *cancellation);
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
    note_qr.clear();
    auto& document = ensure_tui_note_pad(documents, active_pad);
    auto frame = make_note_clear(document.pad_id, document.revision + 1, document.title);
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
  auto prev_button = Button("Prev", [&] {
    const auto ids = sorted_tui_note_pads(documents);
    if (ids.size() <= 1) return;
    const auto it = std::find(ids.begin(), ids.end(), active_pad);
    const auto index = it == ids.end() ? 0 : static_cast<std::size_t>(std::distance(ids.begin(), it));
    switch_active_pad(ids[(index + ids.size() - 1) % ids.size()]);
    wake();
  });
  auto next_button = Button("Next", [&] {
    const auto ids = sorted_tui_note_pads(documents);
    if (ids.size() <= 1) return;
    const auto it = std::find(ids.begin(), ids.end(), active_pad);
    const auto index = it == ids.end() ? 0 : static_cast<std::size_t>(std::distance(ids.begin(), it));
    switch_active_pad(ids[(index + 1) % ids.size()]);
    wake();
  });
  auto new_button = Button("New", [&] {
    flush_active_editor();
    const int number = next_pad_number++;
    const auto pad_id = "pad-" + std::to_string(number);
    const auto title = "Note " + std::to_string(number);
    auto& document = ensure_tui_note_pad(documents, pad_id, title);
    auto frame = make_note_update(document.pad_id, document.revision + 1, document.text, document.title);
    (void)apply_note_update(document, frame);
    if (runtime_connected(runtime)) queue_runtime_frame(runtime, frame);
    switch_active_pad(pad_id);
    {
      std::lock_guard<std::mutex> lock(state.mutex);
      state.latest_local_revision = std::max(state.latest_local_revision, frame.revision);
      state.status = "created " + title;
    }
    wake();
  });
  auto qr_button = Button("QR", [&] {
    if (!note_qr.empty()) {
      note_qr.clear();
      wake();
      return;
    }
    if (editor_text.empty()) {
      std::lock_guard<std::mutex> lock(state.mutex);
      state.status = "note is empty; QR unavailable";
      wake();
      return;
    }
    if (editor_text.size() > kNoteQrMaxBytes) {
      std::lock_guard<std::mutex> lock(state.mutex);
      state.status = "note QR supports up to 1200 bytes";
      wake();
      return;
    }
    try {
      std::ostringstream out;
      print_qrcode(out, editor_text);
      note_qr = out.str();
      std::lock_guard<std::mutex> lock(state.mutex);
      state.status = "QR encodes note text directly";
    } catch (...) {
      std::lock_guard<std::mutex> lock(state.mutex);
      state.status = "QR unavailable for this note";
    }
    wake();
  });
  auto quit_button = Button("Quit", [&] {
    cancellation->request();
    close_runtime(runtime);
    screen.Exit();
  });
  auto controls = Container::Horizontal({prev_button, next_button, new_button, clear_button, qr_button, quit_button});
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
    const auto pad_ids = sorted_tui_note_pads(documents);
    const auto pad_it = documents.find(active_pad);
    const auto pad_title = pad_it == documents.end() ? active_pad : note_pad_title(pad_it->second);
    const auto active_index_it = std::find(pad_ids.begin(), pad_ids.end(), active_pad);
    const auto active_index =
        active_index_it == pad_ids.end() ? 0 : static_cast<std::size_t>(std::distance(pad_ids.begin(), active_index_it));
    header.push_back(text("pad: " + pad_title + " (" + std::to_string(active_index + 1) + "/" +
                          std::to_string(std::max<std::size_t>(pad_ids.size(), 1)) + ")"));
    header.push_back(text("status: " + status));
    if (failed) header.push_back(text("error: " + error) | color(Color::Red));
    if (canceled) header.push_back(text("canceled") | color(Color::Yellow));

    Element editor_panel = editor->Render() | vscroll_indicator | frame | flex;
    if (!note_qr.empty()) {
      Elements qr_rows;
      for (const auto& line : split_lines(note_qr)) {
        qr_rows.push_back(text(line));
      }
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
