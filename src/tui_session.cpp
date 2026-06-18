#include "tui_session.hpp"

#include "doctor.hpp"
#include "tui_advanced.hpp"

namespace kiko {
namespace {

SendConfig make_send_config(const TuiTransferSpec& spec) {
  SendConfig config;
  config.file = spec.path;
  config.relay = spec.relay;
  config.code = spec.code;
  config.relay_pass = spec.relay_pass;
  config.show_qrcode = true;
  apply_network_options_to_send(config, spec.network);
  return config;
}

RecvConfig make_recv_config(const TuiTransferSpec& spec) {
  RecvConfig config;
  config.code = spec.code;
  config.relay = spec.relay;
  config.output_dir = spec.output_dir;
  config.relay_pass = spec.relay_pass;
  apply_network_options_to_recv(config, spec.network);
  return config;
}

std::string diagnose_transfer_failure(const TuiTransferSpec& spec) {
  try {
    DoctorOptions opts;
    opts.relay = spec.relay;
    opts.relay_pass = spec.relay_pass;
    opts.udp_probe = spec.network.udp_probe;
    std::string diagnosis = run_doctor(opts).diagnosis;
    if (diagnosis.empty()) diagnosis = "network check finished (no issues reported)";
    return diagnosis;
  } catch (const std::exception& e) {
    return std::string("network check failed: ") + e.what();
  }
}

void mark_transfer_failed(TuiState& state, const std::exception& error) {
  std::lock_guard<std::mutex> lock(state.mutex);
  state.failed = true;
  state.finished = true;
  state.error_message = error.what();
  state.activity = "error";
  state.doctor_running = true;
}

void finish_failure_diagnosis(TuiState& state, std::string diagnosis) {
  std::lock_guard<std::mutex> lock(state.mutex);
  state.doctor_summary = std::move(diagnosis);
  state.doctor_running = false;
}

}  // namespace

std::thread start_tui_transfer(TuiTransferSpec spec, TuiState& state, std::function<void()> wake) {
  return std::thread([spec = std::move(spec), &state, wake = std::move(wake)]() mutable {
    TuiReporter reporter(state, wake);
    bool failed = false;

    try {
      if (spec.mode == 0) {
        SendConfig config = make_send_config(spec);
        run_send(config, reporter);
      } else {
        RecvConfig config = make_recv_config(spec);
        run_recv(config, reporter);
      }
    } catch (const std::exception& e) {
      mark_transfer_failed(state, e);
      failed = true;
    }

    if (failed) {
      wake();
      finish_failure_diagnosis(state, diagnose_transfer_failure(spec));
    }
    wake();
  });
}

}  // namespace kiko
