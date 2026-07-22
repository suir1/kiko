#include "web/web_job.hpp"

#include "core/cancellation.hpp"
#include "note/note_session.hpp"

#include <mutex>
#include <thread>
#include <utility>

namespace kiko {
namespace {

struct WebNoteRuntime {
  std::shared_ptr<WebReporter> reporter;
  std::shared_ptr<NoteSession> session;
};

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
  if (impl_->worker.joinable()) impl_->worker.join();
}

WebJobSnapshot WebJobStore::snapshot() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->state;
}

void WebJobStore::append_log(const std::string& line) {
  update([&](WebJobSnapshot& state) { state.append_log(line); });
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

  static void reset_task(WebJobStore::Impl& impl) {
    std::lock_guard<std::mutex> lock(impl.mutex);
    impl.note_runtime.reset();
    impl.cancellation.reset();
    impl.state = WebJobSnapshot{};
  }

  template <typename Function>
  static void launch_worker(WebJobStore::Impl& impl, Function&& function) {
    std::lock_guard<std::mutex> lock(impl.mutex);
    if (impl.worker.joinable()) throw KikoError("previous web task worker is still active");
    impl.worker = std::thread(std::forward<Function>(function));
  }

  template <typename Initializer, typename Function>
  static bool start_task(WebJobStore& store, const char* kind,
                         std::shared_ptr<TransferCancellation> cancellation, Initializer&& initialize,
                         Function&& function, std::string& error) {
    try {
      store.join_finished_worker();
      if (!begin_task(*store.impl_, kind, cancellation, error)) return false;

      try {
        std::forward<Initializer>(initialize)();
        launch_worker(*store.impl_,
                      [&store, cancellation = std::move(cancellation),
                       function = std::forward<Function>(function)]() mutable {
                        try {
                          auto success_activity = function();
                          cancellation->requested() ? finish_canceled(store)
                                                    : finish_success(store, success_activity);
                        } catch (const std::exception& e) {
                          cancellation->requested() ? finish_canceled(store) : finish_failed(store, e.what());
                        }
                      });
      } catch (...) {
        reset_task(*store.impl_);
        throw;
      }
      return true;
    } catch (const std::exception& e) {
      error = e.what();
      return false;
    }
  }

  template <typename Config>
  static bool start_transfer(WebJobStore& store, const char* kind, const char* activity, Config config,
                             int (*run)(const Config&, ProgressReporter&), std::string& error) {
    auto cancellation = std::make_shared<TransferCancellation>();
    config.cancellation = cancellation;
    return start_task(
        store, kind, std::move(cancellation), [] {},
        [&store, config = std::move(config), activity = std::string(activity), run]() mutable {
          WebReporter reporter(store);
          const int rc = run(config, reporter);
          if (rc != 0) throw KikoError(activity + " exited with code " + std::to_string(rc));
          return activity + " complete";
        },
        error);
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
};

bool WebJobStore::start_send(SendConfig config, std::string& error) {
  if (config.file.empty()) {
    error = "send path is required";
    return false;
  }
  return Access::start_transfer(*this, "send", "send", std::move(config), run_send, error);
}

bool WebJobStore::start_recv(RecvConfig config, std::string& error) {
  if (config.code.empty()) {
    error = "receive code is required";
    return false;
  }
  return Access::start_transfer(*this, "recv", "receive", std::move(config), run_recv, error);
}

bool WebJobStore::start_doctor(DoctorOptions options, std::string& error) {
  auto cancellation = std::make_shared<TransferCancellation>();
  return Access::start_task(
      *this, "doctor", cancellation, [] {},
      [this, options = std::move(options), cancellation]() mutable {
        append_log("doctor started");
        const auto report = run_doctor(options);
        throw_if_cancelled(cancellation);
        const auto report_json = doctor_report_to_json(report);
        update([&](WebJobSnapshot& state) {
          state.doctor_json = report_json;
          state.doctor_summary = report.diagnosis;
          state.activity = "doctor complete";
        });
        append_log(report.diagnosis);
        return std::string("doctor complete");
      },
      error);
}

bool WebJobStore::start_note(PeerSessionConfig config, std::string& error) {
  try {
    if (config.role == Role::Receiver && config.code.empty()) throw KikoError("note code is required");

    auto cancellation = std::make_shared<TransferCancellation>();
    config.cancellation = cancellation;
    auto runtime = std::make_shared<WebNoteRuntime>();
    runtime->reporter = std::make_shared<WebReporter>(*this);

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
    callbacks.workspace_changed = [this](const NoteSession& session, NoteSessionEvent event,
                                         const NoteFrame& frame) {
      const auto snapshot = session.snapshot();
      const bool remote = event == NoteSessionEvent::RemoteApplied;
      update([&](WebJobSnapshot& state) {
        state.note = snapshot;
        if (remote) {
          state.activity =
              snapshot.active_pad == frame.pad_id ? "remote note update" : "remote note updated " + frame.pad_id;
        } else {
          state.activity = snapshot.synced ? "notepad synced" : "notepad sent";
        }
      });
    };
    runtime->session = std::make_shared<NoteSession>(config, *runtime->reporter, std::move(callbacks));

    return Access::start_task(
        *this, "note", cancellation,
        [this, runtime, role = config.role, code = config.code] {
          std::lock_guard<std::mutex> lock(impl_->mutex);
          impl_->note_runtime = runtime;
          impl_->state.activity = role == Role::Sender ? "hosting notepad" : "joining notepad";
          impl_->state.code = code;
          impl_->state.note = runtime->session->snapshot();
        },
        [cancellation, runtime]() mutable {
          const auto result = runtime->session->run();
          if (result == NoteSessionEnd::Stopped) cancellation->request();
          return std::string("notepad closed");
        },
        error);
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
  return mutate_note(
      [text = std::move(text)](NoteSession& session) mutable {
        return session.update_active(std::move(text));
      },
      [](const NoteSession&) { return "notepad syncing"; }, "notepad is closed", error);
}

bool WebJobStore::clear_note(std::string& error) {
  return mutate_note([](NoteSession& session) { return session.clear_active(); },
                     [](const NoteSession&) { return "notepad syncing"; }, "notepad is closed", error);
}

bool WebJobStore::create_note_pad(std::string& error) {
  return mutate_note([](NoteSession& session) { return session.create_pad(); },
                     [](const NoteSession& session) {
                       return "created " + session.active_document().title;
                     },
                     "notepad is closed", error);
}

bool WebJobStore::select_note_pad(const std::string& pad_id, std::string& error) {
  if (pad_id.empty()) {
    error = "note pad id is required";
    return false;
  }
  return mutate_note([&](NoteSession& session) { return session.select_pad(pad_id); },
                     [&](const NoteSession&) { return "selected " + pad_id; },
                     "note pad not found", error);
}

bool WebJobStore::mutate_note(const std::function<bool(NoteSession&)>& mutation,
                              const std::function<std::string(const NoteSession&)>& activity,
                              const std::string& failure, std::string& error) {
  std::shared_ptr<WebNoteRuntime> runtime;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->state.running || impl_->state.kind != "note" || !impl_->note_runtime) {
      error = "notepad is not running";
      return false;
    }
    runtime = impl_->note_runtime;
  }
  if (!mutation(*runtime->session)) {
    error = failure;
    return false;
  }
  const auto snapshot = runtime->session->snapshot();
  const auto next_activity = activity(*runtime->session);
  update([&](WebJobSnapshot& state) {
    state.note = snapshot;
    state.activity = next_activity;
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

void WebReporter::update_progress_state(UpdateKind kind, const StateMutation& mutation) {
  (void)kind;
  store_.update([&](WebJobSnapshot& state) { (void)mutation(state); });
}

}  // namespace kiko
