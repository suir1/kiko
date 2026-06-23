#include "tui_session.hpp"

#include "core/cancellation.hpp"
#include "diagnostics/doctor.hpp"
#include "tui_advanced.hpp"

namespace kiko {
namespace {

SendConfig make_send_config(const TuiTransferSpec& spec, std::shared_ptr<TransferCancellation> cancellation) {
  SendConfig config;
  config.file = spec.path;
  config.relay = spec.relay;
  config.code = spec.code;
  config.relay_pass = spec.relay_pass;
  config.show_qrcode = true;
  config.cancellation = std::move(cancellation);
  apply_network_options_to_send(config, spec.network);
  return config;
}

RecvConfig make_recv_config(const TuiTransferSpec& spec, std::shared_ptr<TransferCancellation> cancellation) {
  RecvConfig config;
  config.code = spec.code;
  config.relay = spec.relay;
  config.output_dir = spec.output_dir;
  config.relay_pass = spec.relay_pass;
  config.cancellation = std::move(cancellation);
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
  state.end = std::chrono::steady_clock::now();
}

void mark_transfer_canceled(TuiState& state) {
  std::lock_guard<std::mutex> lock(state.mutex);
  state.canceled = true;
  state.finished = true;
  state.failed = false;
  state.error_message.clear();
  state.activity = "canceled";
  state.doctor_running = false;
  state.end = std::chrono::steady_clock::now();
}

void finish_failure_diagnosis(TuiState& state, std::string diagnosis) {
  std::lock_guard<std::mutex> lock(state.mutex);
  state.doctor_summary = std::move(diagnosis);
  state.doctor_running = false;
}

}  // namespace

std::thread start_tui_transfer(TuiTransferSpec spec, TuiState& state, std::function<void()> wake,
                               std::shared_ptr<TransferCancellation> cancellation) {
  return std::thread([spec = std::move(spec), &state, wake = std::move(wake),
                      cancellation = std::move(cancellation)]() mutable {
    TuiReporter reporter(state, wake);
    bool failed = false;

    try {
      if (spec.mode == 0) {
        SendConfig config = make_send_config(spec, cancellation);
        run_send(config, reporter);
      } else {
        RecvConfig config = make_recv_config(spec, cancellation);
        run_recv(config, reporter);
      }
    } catch (const std::exception& e) {
      if (cancellation && cancellation->requested()) {
        mark_transfer_canceled(state);
      } else {
        mark_transfer_failed(state, e);
        failed = true;
      }
    }

    if (failed) {
      wake();
      finish_failure_diagnosis(state, diagnose_transfer_failure(spec));
    }
    wake();
  });
}

}  // namespace kiko
