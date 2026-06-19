#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace kiko {

struct RouteOutcome {
  std::string control_path = "relay";
  std::string data_path = "relay";
  std::string reason;
  bool direct_attempted = false;
  bool lan_upgrade = false;
  bool fallback_ready = true;
  std::string direct_candidate_kind;
  int direct_candidate_priority = -1;
  int direct_elapsed_ms = -1;
  std::string direct_failure_summary;
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

  // The control/data path decision after rendezvous and direct probing.
  virtual void route_outcome(const RouteOutcome& outcome) { (void)outcome; }

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
};

// Prints human-readable lines to stdout, matching kiko's original CLI output.
class CliReporter : public ProgressReporter {
 public:
  void status(const std::string& message) override;
  void connectivity_report(const std::string& report) override;
  void handshake_ok() override;
  void code_ready(const std::string& code, bool show_qrcode = true) override;
  void transfer_overview(std::size_t file_count, std::uint64_t total_bytes) override;
  void file_start(const std::string& path, std::uint64_t size) override;
  void file_complete(const std::string& path, std::uint64_t size, bool verified) override;
  void transfer_complete(std::size_t file_count, std::uint64_t total_bytes) override;
  void route_outcome(const RouteOutcome& outcome) override;

 private:
  std::optional<RouteOutcome> last_route_;
};

}  // namespace kiko
