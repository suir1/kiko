#pragma once

#include "connect/peer_session.hpp"
#include "core/progress_state.hpp"
#include "diagnostics/doctor.hpp"
#include "note/note_workspace.hpp"
#include "transfer/transfer.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace kiko {

class NoteSession;

struct WebJobSnapshot : TransferProgressState {
  WebJobSnapshot() : TransferProgressState(120) {
    // An empty store is idle; the job clock starts in begin_task().
    activity.clear();
    started = {};
  }

  std::string kind;
  bool running = false;
  std::string doctor_json;
  std::string doctor_summary;
  NoteWorkspaceSnapshot note;
  bool note_connected = false;
};

class WebReporter;

class WebJobStore {
 public:
  WebJobStore();
  WebJobStore(const WebJobStore&) = delete;
  WebJobStore& operator=(const WebJobStore&) = delete;
  ~WebJobStore();

  [[nodiscard]] WebJobSnapshot snapshot() const;
  [[nodiscard]] bool start_send(SendConfig config, std::string& error);
  [[nodiscard]] bool start_recv(RecvConfig config, std::string& error);
  [[nodiscard]] bool start_doctor(DoctorOptions options, std::string& error);
  [[nodiscard]] bool start_note(PeerSessionConfig config, std::string& error);
  [[nodiscard]] bool update_note(std::string text, std::string& error);
  [[nodiscard]] bool clear_note(std::string& error);
  [[nodiscard]] bool create_note_pad(std::string& error);
  [[nodiscard]] bool select_note_pad(const std::string& pad_id, std::string& error);

  void cancel();
  void join_finished_worker();

 private:
  friend class WebReporter;

  void append_log(const std::string& line);
  void update(const std::function<void(WebJobSnapshot&)>& fn);
  [[nodiscard]] bool mutate_note(const std::function<bool(NoteSession&)>& mutation,
                                 const std::function<std::string(const NoteSession&)>& activity,
                                 const std::string& failure, std::string& error);

  struct Impl;
  struct Access;
  std::unique_ptr<Impl> impl_;
};

class WebReporter : public ProgressStateReporter {
 public:
  explicit WebReporter(WebJobStore& store);

 protected:
  void update_progress_state(UpdateKind kind, const StateMutation& mutation) override;

 private:
  WebJobStore& store_;
};

}  // namespace kiko
