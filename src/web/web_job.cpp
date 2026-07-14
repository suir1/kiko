#include "web/web_job.hpp"

#include "core/cancellation.hpp"
#include "note/note_session.hpp"
#include "note/note_workspace.hpp"

#include <mutex>
#include <thread>
#include <utility>

namespace kiko {
namespace {

struct WebNoteRuntime {
  NoteWorkspace workspace;
  std::shared_ptr<WebReporter> reporter;
  std::shared_ptr<NoteSession> session;
};

void apply_note_snapshot(WebJobSnapshot& state, const NoteWorkspaceSnapshot& snapshot) {
  state.note_active_pad = snapshot.active_pad;
  state.note_pads.clear();
  state.note_pads.reserve(snapshot.documents.size());
  for (const auto& document : snapshot.documents) {
    state.note_pads.push_back(
        WebNotePadSnapshot{document.pad_id, document.title.empty() ? document.pad_id : document.title,
                           document.revision});
    if (document.pad_id == snapshot.active_pad) {
      state.note_text = document.text;
      state.note_revision = document.revision;
    }
  }
  state.note_local_revision = snapshot.latest_local_revision;
  state.note_acked_revision = snapshot.last_acked_revision;
  state.note_synced = snapshot.synced;
}

}  // namespace

struct WebJobStore::Impl {
  mutable std::mutex mutex;
  WebJobSnapshot state;
  std::shared_ptr<TransferCancellation> cancellation;
  std::shared_ptr<WebNoteRuntime> note_runtime;
  std::thread worker;
};

WebJobStore::WebJobStore() : impl_(std::make_unique<Impl>()) {}

WebJobStore::~WebJobStore() {
  cancel();
  std::thread worker;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->worker.joinable()) worker = std::move(impl_->worker);
  }
  if (worker.joinable()) worker.join();
}

WebJobSnapshot WebJobStore::snapshot() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->state;
}

void WebJobStore::append_log(const std::string& line) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->state.append_log(line);
}

void WebJobStore::update(const std::function<void(WebJobSnapshot&)>& fn) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  fn(impl_->state);
}

struct WebJobStore::Access {
  static bool begin_task(WebJobStore::Impl& impl, const std::string& kind,
                         std::shared_ptr<TransferCancellation> cancellation, std::string& error) {
    std::lock_guard<std::mutex> lock(impl.mutex);
    if (impl.state.running) {
      error = "another kiko web task is already running";
      return false;
    }
    impl.note_runtime.reset();
    impl.state = WebJobSnapshot{};
    impl.state.kind = kind;
    impl.state.running = true;
    impl.state.activity = "starting";
    impl.state.started = std::chrono::steady_clock::now();
    impl.cancellation = std::move(cancellation);
    return true;
  }

  template <typename Function>
  static void launch_worker(WebJobStore::Impl& impl, Function&& function) {
    std::lock_guard<std::mutex> lock(impl.mutex);
    try {
      if (impl.worker.joinable()) throw KikoError("previous web task worker is still active");
      impl.worker = std::thread(std::forward<Function>(function));
    } catch (...) {
      impl.note_runtime.reset();
      impl.cancellation.reset();
      impl.state = WebJobSnapshot{};
      throw;
    }
  }

  static void finish_success(WebJobStore& store, const std::string& activity) {
    store.update([&](WebJobSnapshot& state) {
      state.running = false;
      if (!state.failed && !state.canceled) {
        state.finish_success(activity);
      }
    });
  }

  static void finish_failed(WebJobStore& store, const std::string& error) {
    store.update([&](WebJobSnapshot& state) {
      state.running = false;
      state.finish_failed(error);
    });
  }

  static void finish_canceled(WebJobStore& store) {
    store.update([](WebJobSnapshot& state) {
      state.running = false;
      state.finish_canceled();
    });
  }

  static std::shared_ptr<WebNoteRuntime> current_note_runtime(WebJobStore::Impl& impl, std::string& error) {
    std::lock_guard<std::mutex> lock(impl.mutex);
    if (!impl.state.running || impl.state.kind != "note" || !impl.note_runtime) {
      error = "notepad is not running";
      return {};
    }
    return impl.note_runtime;
  }
};

bool WebJobStore::start_send(SendConfig config, std::string& error) {
  try {
    join_finished_worker();
    if (config.file.empty()) throw KikoError("send path is required");
    auto cancellation = std::make_shared<TransferCancellation>();
    config.cancellation = cancellation;
    if (!Access::begin_task(*impl_, "send", cancellation, error)) return false;

    Access::launch_worker(
        *impl_, [this, config = std::move(config), cancellation = std::move(cancellation)]() mutable {
          WebReporter reporter(*this);
          try {
            const int rc = run_send(config, reporter);
            if (rc == 0) Access::finish_success(*this, "send complete");
            else if (cancellation->requested()) Access::finish_canceled(*this);
            else Access::finish_failed(*this, "send exited with code " + std::to_string(rc));
          } catch (const std::exception& e) {
            cancellation->requested() ? Access::finish_canceled(*this) : Access::finish_failed(*this, e.what());
          }
        });
    return true;
  } catch (const std::exception& e) {
    error = e.what();
    return false;
  }
}

bool WebJobStore::start_recv(RecvConfig config, std::string& error) {
  try {
    join_finished_worker();
    if (config.code.empty()) throw KikoError("receive code is required");
    auto cancellation = std::make_shared<TransferCancellation>();
    config.cancellation = cancellation;
    if (!Access::begin_task(*impl_, "recv", cancellation, error)) return false;

    Access::launch_worker(
        *impl_, [this, config = std::move(config), cancellation = std::move(cancellation)]() mutable {
          WebReporter reporter(*this);
          try {
            const int rc = run_recv(config, reporter);
            if (rc == 0) Access::finish_success(*this, "receive complete");
            else if (cancellation->requested()) Access::finish_canceled(*this);
            else Access::finish_failed(*this, "receive exited with code " + std::to_string(rc));
          } catch (const std::exception& e) {
            cancellation->requested() ? Access::finish_canceled(*this) : Access::finish_failed(*this, e.what());
          }
        });
    return true;
  } catch (const std::exception& e) {
    error = e.what();
    return false;
  }
}

bool WebJobStore::start_doctor(DoctorOptions options, std::string& error) {
  try {
    join_finished_worker();
    auto cancellation = std::make_shared<TransferCancellation>();
    if (!Access::begin_task(*impl_, "doctor", cancellation, error)) return false;

    Access::launch_worker(
        *impl_, [this, options = std::move(options), cancellation = std::move(cancellation)]() mutable {
          try {
            append_log("doctor started");
            const auto report = run_doctor(options);
            if (cancellation->requested()) {
              Access::finish_canceled(*this);
              return;
            }
            const auto report_json = doctor_report_to_json(report);
            update([&](WebJobSnapshot& state) {
              state.doctor_json = report_json;
              state.doctor_summary = report.diagnosis;
              state.activity = "doctor complete";
            });
            append_log(report.diagnosis);
            Access::finish_success(*this, "doctor complete");
          } catch (const std::exception& e) {
            cancellation->requested() ? Access::finish_canceled(*this) : Access::finish_failed(*this, e.what());
          }
        });
    return true;
  } catch (const std::exception& e) {
    error = e.what();
    return false;
  }
}

bool WebJobStore::start_note(NoteConfig config, std::string& error) {
  try {
    join_finished_worker();
    if (config.role == Role::Receiver && config.code.empty()) throw KikoError("note code is required");

    auto cancellation = std::make_shared<TransferCancellation>();
    config.cancellation = cancellation;
    auto runtime = std::make_shared<WebNoteRuntime>();
    runtime->reporter = std::make_shared<WebReporter>(*this);
    std::weak_ptr<WebNoteRuntime> weak_runtime = runtime;

    NoteSessionCallbacks callbacks;
    callbacks.connected = [this](const NoteSessionInfo& info) {
      update([&](WebJobSnapshot& state) {
        state.note_connected = true;
        state.pairing_code_ready(info.code);
        if (state.route_summary.empty()) state.route_summary = format_route_outcome_label(info.outcome);
        state.activity = "notepad connected";
      });
      append_log("notepad connected via " + info.outcome.data_path);
    };
    callbacks.frame_received = [this, weak_runtime](const NoteFrame& frame) {
      const auto runtime = weak_runtime.lock();
      if (!runtime) return;
      if (frame.type == NoteFrameType::Ack) {
        runtime->workspace.acknowledge(frame);
        const auto snapshot = runtime->workspace.snapshot();
        update([&](WebJobSnapshot& state) {
          apply_note_snapshot(state, snapshot);
          state.activity = snapshot.synced ? "notepad synced" : "notepad sent";
        });
        return;
      }
      if (!runtime->workspace.apply_remote(frame)) return;
      const auto snapshot = runtime->workspace.snapshot();
      update([&](WebJobSnapshot& state) {
        apply_note_snapshot(state, snapshot);
        state.activity =
            snapshot.active_pad == frame.pad_id ? "remote note update" : "remote note updated " + frame.pad_id;
      });
    };
    callbacks.frame_sent = [this, weak_runtime](const NoteFrame& frame) {
      if (frame.type != NoteFrameType::Update && frame.type != NoteFrameType::Clear) return;
      const auto runtime = weak_runtime.lock();
      if (!runtime) return;
      const auto snapshot = runtime->workspace.snapshot();
      update([&](WebJobSnapshot& state) {
        apply_note_snapshot(state, snapshot);
        state.activity = snapshot.synced ? "notepad synced" : "notepad sent";
      });
    };
    runtime->session = std::make_shared<NoteSession>(config, *runtime->reporter, std::move(callbacks));

    if (!Access::begin_task(*impl_, "note", cancellation, error)) return false;
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      impl_->note_runtime = runtime;
      impl_->state.activity = config.role == Role::Sender ? "hosting notepad" : "joining notepad";
      impl_->state.code = config.code;
      apply_note_snapshot(impl_->state, runtime->workspace.snapshot());
    }

    Access::launch_worker(
        *impl_, [this, cancellation = std::move(cancellation), runtime = std::move(runtime)]() mutable {
          try {
            const auto result = runtime->session->run();
            if (result == NoteSessionEnd::Stopped || cancellation->requested()) {
              Access::finish_canceled(*this);
            } else {
              Access::finish_success(*this, "notepad closed");
            }
          } catch (const std::exception& e) {
            cancellation->requested() ? Access::finish_canceled(*this) : Access::finish_failed(*this, e.what());
          }
        });
    return true;
  } catch (const std::exception& e) {
    error = e.what();
    return false;
  }
}

bool WebJobStore::update_note(std::string text, std::string& error) {
  if (text.size() > kNoteMaxBytes) {
    error = "note text exceeds 1 MiB limit";
    return false;
  }
  auto runtime = Access::current_note_runtime(*impl_, error);
  if (!runtime) return false;

  if (!runtime->session->send(runtime->workspace.update_active(std::move(text)))) {
    error = "notepad is closed";
    return false;
  }
  const auto snapshot = runtime->workspace.snapshot();
  update([&](WebJobSnapshot& state) {
    apply_note_snapshot(state, snapshot);
    state.activity = "notepad syncing";
  });
  return true;
}

bool WebJobStore::clear_note(std::string& error) {
  auto runtime = Access::current_note_runtime(*impl_, error);
  if (!runtime) return false;
  if (!runtime->session->send(runtime->workspace.clear_active())) {
    error = "notepad is closed";
    return false;
  }
  const auto snapshot = runtime->workspace.snapshot();
  update([&](WebJobSnapshot& state) {
    apply_note_snapshot(state, snapshot);
    state.activity = "notepad syncing";
  });
  return true;
}

bool WebJobStore::create_note_pad(std::string& error) {
  auto runtime = Access::current_note_runtime(*impl_, error);
  if (!runtime) return false;
  auto frame = runtime->workspace.create_pad();
  const auto title = frame.title;
  if (!runtime->session->send(std::move(frame))) {
    error = "notepad is closed";
    return false;
  }
  const auto snapshot = runtime->workspace.snapshot();
  update([&](WebJobSnapshot& state) {
    apply_note_snapshot(state, snapshot);
    state.activity = "created " + title;
  });
  return true;
}

bool WebJobStore::select_note_pad(const std::string& pad_id, std::string& error) {
  if (pad_id.empty()) {
    error = "note pad id is required";
    return false;
  }
  auto runtime = Access::current_note_runtime(*impl_, error);
  if (!runtime) return false;
  if (!runtime->workspace.select_pad(pad_id)) {
    error = "note pad not found";
    return false;
  }
  const auto snapshot = runtime->workspace.snapshot();
  update([&](WebJobSnapshot& state) {
    apply_note_snapshot(state, snapshot);
    state.activity = "selected " + pad_id;
  });
  return true;
}

void WebJobStore::cancel() {
  std::shared_ptr<TransferCancellation> cancellation;
  std::shared_ptr<WebNoteRuntime> note_runtime;
  bool should_log = false;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    cancellation = impl_->cancellation;
    note_runtime = impl_->note_runtime;
    if (impl_->state.running) {
      should_log = impl_->state.activity != "cancel requested";
      impl_->state.activity = "cancel requested";
    }
  }
  if (should_log) append_log("cancel requested");
  if (cancellation) cancellation->request();
  if (note_runtime && note_runtime->session) note_runtime->session->request_stop();
}

void WebJobStore::join_finished_worker() {
  std::thread worker;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->state.running || !impl_->worker.joinable()) return;
    worker = std::move(impl_->worker);
  }
  if (worker.joinable()) worker.join();
}

WebReporter::WebReporter(WebJobStore& store) : store_(store) {}

void WebReporter::status(const std::string& message) {
  store_.update([&](WebJobSnapshot& state) { state.status(message); });
}

void WebReporter::connectivity_report(const std::string& report) {
  store_.update([&](WebJobSnapshot& state) { state.connectivity_report(report); });
}

void WebReporter::route_phase(RoutePhase phase, const RoutePhaseDetail& detail) {
  store_.update([&](WebJobSnapshot& state) { state.route_phase_changed(phase, detail); });
}

void WebReporter::route_outcome(const RouteOutcome& outcome) {
  store_.update([&](WebJobSnapshot& state) { state.route_selected(outcome); });
}

void WebReporter::route_timing(const RouteTiming& timing) {
  store_.update([&](WebJobSnapshot& state) { state.route_timing_recorded(timing); });
}

void WebReporter::handshake_ok() {
  store_.update([](WebJobSnapshot& state) { state.handshake_completed(); });
}

void WebReporter::code_ready(const std::string& code, bool show_qrcode) {
  (void)show_qrcode;
  store_.update([&](WebJobSnapshot& state) { state.pairing_code_ready(code); });
}

void WebReporter::transfer_overview(std::size_t file_count, std::uint64_t total_bytes) {
  store_.update([&](WebJobSnapshot& state) { state.transfer_overview_received(file_count, total_bytes); });
}

void WebReporter::receive_plan(const ReceivePlanSummary& summary) {
  store_.update([&](WebJobSnapshot& state) { state.receive_plan_ready(summary); });
}

void WebReporter::file_start(const std::string& path, std::uint64_t size) {
  store_.update([&](WebJobSnapshot& state) { state.file_started(path, size); });
}

void WebReporter::file_advance(std::uint64_t bytes_delta) {
  store_.update([&](WebJobSnapshot& state) { (void)state.file_advanced(bytes_delta); });
}

void WebReporter::file_resume(const std::string& path, std::uint64_t offset, std::uint64_t size) {
  store_.update([&](WebJobSnapshot& state) { state.file_resumed(path, offset, size); });
}

void WebReporter::file_complete(const std::string& path, std::uint64_t size, bool verified) {
  (void)path;
  (void)size;
  (void)verified;
  store_.update([](WebJobSnapshot& state) { state.file_completed(); });
}

void WebReporter::transfer_complete(std::size_t file_count, std::uint64_t total_bytes) {
  store_.update([&](WebJobSnapshot& state) { state.transfer_completed(file_count, total_bytes); });
}

void WebReporter::transfer_retry(int next_attempt, int max_attempts, const std::string& reason) {
  store_.update([&](WebJobSnapshot& state) { state.transfer_retrying(next_attempt, max_attempts, reason); });
}

void WebReporter::transfer_retry_delay(int next_attempt, int max_attempts, std::chrono::milliseconds delay) {
  if (delay.count() <= 0) return;
  store_.update([&](WebJobSnapshot& state) { state.transfer_retry_waiting(next_attempt, max_attempts, delay); });
}

}  // namespace kiko
