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

  struct Impl;
  struct Access;
  std::unique_ptr<Impl> impl_;
};

class WebReporter : public ProgressReporter {
 public:
  explicit WebReporter(WebJobStore& store);

  void status(const std::string& message) override;
  void connectivity_report(const std::string& report) override;
  void route_phase(RoutePhase phase, const RoutePhaseDetail& detail) override;
  void route_outcome(const RouteOutcome& outcome) override;
  void route_timing(const RouteTiming& timing) override;
  void handshake_ok() override;
  void code_ready(const std::string& code, bool show_qrcode = true) override;
  void transfer_overview(std::size_t file_count, std::uint64_t total_bytes) override;
  void receive_plan(const ReceivePlanSummary& summary) override;
  void file_start(const std::string& path, std::uint64_t size) override;
  void file_advance(std::uint64_t bytes_delta) override;
  void file_resume(const std::string& path, std::uint64_t offset, std::uint64_t size) override;
  void file_complete(const std::string& path, std::uint64_t size, bool verified) override;
  void transfer_complete(std::size_t file_count, std::uint64_t total_bytes) override;
  void transfer_retry(int next_attempt, int max_attempts, const std::string& reason) override;
  void transfer_retry_delay(int next_attempt, int max_attempts, std::chrono::milliseconds delay) override;

 private:
  WebJobStore& store_;
};

}  // namespace kiko
