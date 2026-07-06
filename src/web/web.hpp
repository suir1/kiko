#pragma once

#include "core/common.hpp"
#include "core/progress.hpp"
#include "platform/user_config.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace kiko {

struct WebOptions {
  Endpoint listen{"127.0.0.1", 0};
  Endpoint relay;
  std::optional<std::string> relay_pass;
  bool open_browser = true;
  UserConfig user_config;
};

struct WebDirectoryEntry {
  std::string label;
  std::filesystem::path path;
  bool is_dir = false;
  bool selectable = false;
  bool parent = false;
  bool select_here = false;
  std::filesystem::file_time_type modified{};
  bool has_modified = false;
};

enum class WebPickMode { FileOrDirectory, DirectoryOnly };
enum class WebBrowserSort { Name, ModifiedDesc };

[[nodiscard]] bool web_listen_is_loopback(const Endpoint& endpoint);
[[nodiscard]] std::string generate_web_token();
[[nodiscard]] std::vector<WebDirectoryEntry> list_web_directory(const std::filesystem::path& dir,
                                                               WebPickMode mode,
                                                               WebBrowserSort sort,
                                                               const std::string& filter = {});

struct WebJobSnapshot {
  std::string kind;
  bool running = false;
  bool finished = false;
  bool failed = false;
  bool canceled = false;
  std::string error;
  std::string activity;
  std::string code;
  std::string current_file;
  std::uint64_t current_done = 0;
  std::uint64_t current_size = 0;
  std::uint64_t overall_done = 0;
  std::uint64_t overall_total = 0;
  std::size_t files_done = 0;
  std::size_t files_total = 0;
  std::string route_phase;
  std::string route_summary;
  std::string route_timing;
  std::string doctor_json;
  std::string doctor_summary;
  std::string note_text;
  std::uint64_t note_revision = 0;
  std::uint64_t note_local_revision = 0;
  std::uint64_t note_acked_revision = 0;
  bool note_connected = false;
  std::vector<std::string> logs;
  std::chrono::steady_clock::time_point started{};
  std::optional<std::chrono::steady_clock::time_point> ended;
};

class WebJobStore;

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

int run_web_console(const WebOptions& options);

}  // namespace kiko
