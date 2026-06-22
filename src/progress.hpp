#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace kiko {

enum class RoutePhase {
  Rendezvous,
  RelayStandby,
  DirectProbing,
  RelayCommitted,
  Securing,
};

struct RoutePhaseDetail {
  std::string message;
  std::string reason;
  bool relay_fallback_ready = false;
};

struct RouteOutcome {
  std::string control_path = "relay";
  std::string data_path = "relay";
  std::string reason;
  bool direct_attempted = false;
  bool lan_upgrade = false;
  std::string direct_candidate_kind;
  std::string direct_candidate_endpoint;
  std::string direct_candidate_family;
  std::string direct_candidate_scope;
  int direct_candidate_priority = -1;
  int direct_elapsed_ms = -1;
  std::string direct_failure_summary;
};

struct RouteTiming {
  int rendezvous_ms = -1;
  int direct_probe_ms = -1;
  int relay_commit_ms = -1;
  int securing_ms = -1;
};

// Decouples the transfer core from any particular front-end. The CLI prints
// lines; the TUI updates widgets. The transfer logic only emits these events.
class ProgressReporter {
 public:
  virtual ~ProgressReporter() = default;

  // A coarse status/log line (e.g. "direct connection established").
  virtual void status(const std::string& message) { (void)message; }

  // AdaptivePuncher observations after a direct-connect attempt.
  virtual void connectivity_report(const std::string& report) { (void)report; }

  // Structured connection-route phase for front-ends that should not parse
  // free-form status lines.
  virtual void route_phase(RoutePhase phase, const RoutePhaseDetail& detail) {
    (void)phase;
    (void)detail;
  }

  // The control/data path decision after rendezvous and direct probing.
  virtual void route_outcome(const RouteOutcome& outcome) { (void)outcome; }

  // Route timing slices, emitted once a route is selected or secured.
  virtual void route_timing(const RouteTiming& timing) { (void)timing; }

  // The PAKE handshake succeeded and an encrypted channel is established.
  virtual void handshake_ok() {}

  // The sender's pairing code is ready to be shared.
  virtual void code_ready(const std::string& code, bool show_qrcode = true) {
    (void)code;
    (void)show_qrcode;
  }

  // The peer summary became known (file count and total bytes).
  virtual void transfer_overview(std::size_t file_count, std::uint64_t total_bytes) {
    (void)file_count;
    (void)total_bytes;
  }

  // A file transfer started.
  virtual void file_start(const std::string& path, std::uint64_t size) {
    (void)path;
    (void)size;
  }

  // bytes_delta more plaintext bytes were processed for the current file.
  virtual void file_advance(std::uint64_t bytes_delta) { (void)bytes_delta; }

  // The current file will continue from an already verified prefix. This is
  // informational; callers still emit file_advance(offset) to account for the
  // resumed bytes in progress totals.
  virtual void file_resume(const std::string& path, std::uint64_t offset, std::uint64_t size) {
    (void)path;
    (void)offset;
    (void)size;
  }

  // The current file finished; verified indicates SHA-256 match (receiver) or
  // simply that it was fully sent (sender, always true).
  virtual void file_complete(const std::string& path, std::uint64_t size, bool verified) {
    (void)path;
    (void)size;
    (void)verified;
  }

  // The whole transfer finished.
  virtual void transfer_complete(std::size_t file_count, std::uint64_t total_bytes) {
    (void)file_count;
    (void)total_bytes;
  }

  // The current connection failed but the transfer will reconnect and rely on
  // the normal resume protocol to continue any partial files.
  virtual void transfer_retry(int next_attempt, int max_attempts, const std::string& reason) {
    status("connection lost, retrying " + std::to_string(next_attempt) + "/" + std::to_string(max_attempts) +
           "; resume will continue verified partial files; reason: " + reason);
  }
};

// Prints human-readable lines to stdout, matching kiko's original CLI output.
class CliReporter : public ProgressReporter {
 public:
  void status(const std::string& message) override;
  void connectivity_report(const std::string& report) override;
  void route_phase(RoutePhase phase, const RoutePhaseDetail& detail) override;
  void handshake_ok() override;
  void code_ready(const std::string& code, bool show_qrcode = true) override;
  void transfer_overview(std::size_t file_count, std::uint64_t total_bytes) override;
  void file_start(const std::string& path, std::uint64_t size) override;
  void file_resume(const std::string& path, std::uint64_t offset, std::uint64_t size) override;
  void file_complete(const std::string& path, std::uint64_t size, bool verified) override;
  void transfer_complete(std::size_t file_count, std::uint64_t total_bytes) override;
  void route_outcome(const RouteOutcome& outcome) override;
  void route_timing(const RouteTiming& timing) override;

 private:
  std::optional<RouteOutcome> last_route_;
};

}  // namespace kiko
