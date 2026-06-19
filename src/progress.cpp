#include "progress.hpp"

#include "platform.hpp"
#include "qrcode_print.hpp"

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

}  // namespace

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
