#include "core/progress.hpp"

#include "platform/platform.hpp"
#include "core/qrcode_print.hpp"

#include <iostream>

namespace kiko {
namespace {

const char* route_phase_name(RoutePhase phase) {
  switch (phase) {
    case RoutePhase::Rendezvous:
      return "rendezvous";
    case RoutePhase::RelayStandby:
      return "relay_standby";
    case RoutePhase::DirectProbing:
      return "direct_probing";
    case RoutePhase::RelayCommitted:
      return "relay_committed";
    case RoutePhase::Securing:
      return "securing";
  }
  return "unknown";
}

std::string route_phase_summary(RoutePhase phase, const RoutePhaseDetail& detail) {
  std::string line = "route phase: ";
  line += route_phase_name(phase);
  if (!detail.reason.empty()) line += " reason=" + detail.reason;
  if (detail.relay_fallback_ready) line += " relay_fallback=ready";
  if (!detail.message.empty()) line += " message=" + detail.message;
  return line;
}

std::string route_outcome_summary(const RouteOutcome& outcome) {
  std::string line = "route summary: control=" + outcome.control_path + " data=" + outcome.data_path;
  if (!outcome.reason.empty()) line += " reason=" + outcome.reason;
  line += " direct_attempted=" + std::string(outcome.direct_attempted ? "true" : "false");
  if (outcome.data_path == "direct" && !outcome.direct_candidate_kind.empty()) {
    line += " candidate=" + outcome.direct_candidate_kind;
    if (outcome.direct_candidate_priority >= 0) {
      line += "#" + std::to_string(outcome.direct_candidate_priority);
    }
    if (!outcome.direct_candidate_endpoint.empty()) line += " endpoint=" + outcome.direct_candidate_endpoint;
    if (!outcome.direct_candidate_family.empty()) line += " family=" + outcome.direct_candidate_family;
    if (!outcome.direct_candidate_scope.empty()) line += " scope=" + outcome.direct_candidate_scope;
    if (outcome.direct_elapsed_ms >= 0) line += " elapsed=" + std::to_string(outcome.direct_elapsed_ms) + "ms";
  }
  if (outcome.data_path == "relay" && !outcome.direct_attempted) {
    line += " direct=not_attempted";
  } else if (outcome.data_path == "relay" && !outcome.direct_failure_summary.empty()) {
    line += " direct_failure=" + outcome.direct_failure_summary;
  }
  if (outcome.data_path == "relay") {
    line += " lan_upgrade=" + std::string(outcome.lan_upgrade ? "true" : "false");
  }
  return line;
}

void append_timing_field(std::string& line, const std::string& name, int value_ms) {
  if (value_ms >= 0) line += " " + name + "_ms=" + std::to_string(value_ms);
}

std::string route_timing_summary(const RouteTiming& timing) {
  std::string line = "route timing:";
  append_timing_field(line, "rendezvous", timing.rendezvous_ms);
  append_timing_field(line, "direct_probe", timing.direct_probe_ms);
  append_timing_field(line, "relay_commit", timing.relay_commit_ms);
  append_timing_field(line, "securing", timing.securing_ms);
  return line;
}

void append_counter_field(std::string& line, const std::string& name, std::uint64_t value) {
  if (value > 0) line += " " + name + "=" + std::to_string(value);
}

void append_duration_field(std::string& line, const std::string& name, std::int64_t value_ms) {
  if (value_ms > 0) line += " " + name + "_ms=" + std::to_string(value_ms);
}

std::string transfer_timing_summary(const TransferTiming& timing) {
  std::string line = "transfer timing:";
  if (!timing.mode.empty()) line += " mode=" + timing.mode;
  append_counter_field(line, "bytes", timing.payload_bytes);
  append_counter_field(line, "frames", timing.frame_count);
  append_duration_field(line, "send_frame", timing.send_frame_ms);
  if (timing.max_send_frame_ms > 0) line += " max_send_frame_ms=" + std::to_string(timing.max_send_frame_ms);
  append_duration_field(line, "disk_read", timing.disk_read_ms);
  append_duration_field(line, "disk_write", timing.disk_write_ms);
  append_duration_field(line, "compress", timing.compress_ms);
  append_duration_field(line, "decompress", timing.decompress_ms);
  append_duration_field(line, "hash", timing.hash_ms);
  append_counter_field(line, "mux_max_pending_bytes", timing.mux_max_pending_bytes);
  append_duration_field(line, "mux_backpressure_wait", timing.mux_backpressure_wait_ms);
  append_counter_field(line, "mux_backpressure_waits", timing.mux_backpressure_wait_count);
  if (timing.mux_channels > 0) line += " mux_channels=" + std::to_string(timing.mux_channels);
  return line;
}

}  // namespace

std::string format_receive_plan_summary(const ReceivePlanSummary& summary) {
  std::string line = "receive plan: " + std::to_string(summary.item_count) + " item(s), " +
                     std::to_string(summary.total_bytes) + " bytes";
  if (summary.skip_count > 0) line += ", skip=" + std::to_string(summary.skip_count);
  if (summary.rename_count > 0) line += ", rename=" + std::to_string(summary.rename_count);
  if (summary.overwrite_count > 0) line += ", overwrite=" + std::to_string(summary.overwrite_count);
  if (summary.resume_count > 0) {
    line += ", resume=" + std::to_string(summary.resume_count) + " (" +
            std::to_string(summary.resume_bytes) + " bytes)";
  }
  if (summary.skip_bytes > 0) line += ", skipped-bytes=" + std::to_string(summary.skip_bytes);
  return line;
}

std::string format_transfer_retry_summary(int next_attempt, int max_attempts, const std::string& reason) {
  return "connection lost, retrying " + std::to_string(next_attempt) + "/" + std::to_string(max_attempts) +
         "; resume will continue verified partial files; reason: " + reason;
}

std::string format_transfer_retry_delay_summary(int next_attempt, int max_attempts,
                                                std::chrono::milliseconds delay) {
  return "reconnect in " + std::to_string(delay.count()) + "ms before attempt " +
         std::to_string(next_attempt) + "/" + std::to_string(max_attempts);
}

void CliReporter::status(const std::string& message) { std::cout << message << "\n"; }

void CliReporter::connectivity_report(const std::string& report) {
  std::cout << "punch report:\n" << report;
}

void CliReporter::route_phase(RoutePhase phase, const RoutePhaseDetail& detail) {
  std::cout << route_phase_summary(phase, detail) << "\n";
}

void CliReporter::route_outcome(const RouteOutcome& outcome) {
  last_route_ = outcome;
  std::cout << route_outcome_summary(outcome) << "\n";
}

void CliReporter::route_timing(const RouteTiming& timing) { std::cout << route_timing_summary(timing) << "\n"; }

void CliReporter::transfer_timing(const TransferTiming& timing) {
  std::cout << transfer_timing_summary(timing) << "\n";
}

void CliReporter::handshake_ok() { std::cout << "pake handshake ok\n"; }

void CliReporter::code_ready(const std::string& code, bool show_qrcode) {
  std::cout << "code: " << code << "\n";
  if (show_qrcode && stdin_is_tty()) print_qrcode(std::cout, code);
}

void CliReporter::transfer_overview(std::size_t file_count, std::uint64_t total_bytes) {
  std::cout << "incoming " << file_count << " file(s), " << total_bytes << " bytes\n";
}

void CliReporter::file_start(const std::string& path, std::uint64_t size) {
  std::cout << "-> " << path << " (" << size << " bytes)\n";
}

void CliReporter::file_resume(const std::string& path, std::uint64_t offset, std::uint64_t size) {
  std::cout << "resuming " << path << " from " << offset << "/" << size << " bytes\n";
}

void CliReporter::file_complete(const std::string& path, std::uint64_t size, bool verified) {
  if (verified) {
    std::cout << "received " << path << " (" << size << " bytes, sha256 ok)\n";
  } else {
    std::cout << "sent " << path << " (" << size << " bytes)\n";
  }
}

void CliReporter::transfer_complete(std::size_t file_count, std::uint64_t total_bytes) {
  std::cout << "transfer complete: " << file_count << " file(s), " << total_bytes << " bytes\n";
  if (last_route_) std::cout << route_outcome_summary(*last_route_) << "\n";
}

}  // namespace kiko
