#pragma once

#include "core/progress_state.hpp"
#include "diagnostics/doctor.hpp"
#include "note/notepad.hpp"
#include "transfer/transfer.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace kiko {

class NoteSession;

struct WebNotePadSnapshot {
  std::string id;
  std::string title;
  std::uint64_t revision = 0;
};

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
  std::string note_text;
  std::string note_active_pad = "main";
  std::vector<WebNotePadSnapshot> note_pads;
  std::uint64_t note_revision = 0;
  std::uint64_t note_local_revision = 0;
  std::uint64_t note_acked_revision = 0;
  bool note_connected = false;
  bool note_synced = false;
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
  [[nodiscard]] bool start_note(NoteConfig config, std::string& error);
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
