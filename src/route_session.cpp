#include "route_session.hpp"

#include "platform.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <exception>
#include <future>
#include <sstream>
#include <thread>

namespace kiko {
namespace {

void report_route_result(ProgressReporter& reporter, const std::string& path, const std::string& reason,
                         bool direct_attempted, bool allow_lan_upgrade = false) {
  reporter.status("route result: path=" + path + " reason=" + reason +
                  " direct_attempted=" + (direct_attempted ? "true" : "false") +
                  " lan_upgrade=" + (allow_lan_upgrade ? "true" : "false"));
}

void report_route_phase(ProgressReporter& reporter, RoutePhase phase, std::string message, std::string reason = {},
                        bool relay_fallback_ready = false) {
  reporter.route_phase(phase, RoutePhaseDetail{std::move(message), std::move(reason), relay_fallback_ready});
}

int elapsed_ms_since(std::chrono::steady_clock::time_point start) {
  const auto elapsed = std::chrono::steady_clock::now() - start;
  return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

std::string count_map_summary(const std::map<std::string, int>& values) {
  std::ostringstream oss;
  bool first = true;
  for (const auto& [key, count] : values) {
    if (!first) oss << ",";
    first = false;
    oss << key << "=" << count;
  }
  return oss.str();
}

void report_route_detail(ProgressReporter& reporter, const PunchStats& stats) {
  if (!stats.attempted) {
    reporter.status("route detail: direct_not_attempted");
    return;
  }

  if (stats.direct_ok) {
    std::string line = "route detail: direct_success";
    if (!stats.successful_candidate_kind.empty()) {
      line += " kind=" + stats.successful_candidate_kind;
      line += " priority=" + std::to_string(stats.successful_candidate_priority);
      if (stats.successful_elapsed_ms >= 0) line += " elapsed_ms=" + std::to_string(stats.successful_elapsed_ms);
    }
    reporter.status(line);
    return;
  }

  std::string line = "route detail: direct_failed";
  if (!stats.failures.empty()) line += " failures=" + count_map_summary(stats.failures);
  if (!stats.candidate_failures_by_kind.empty()) {
    line += " candidate_kinds=" + count_map_summary(stats.candidate_failures_by_kind);
  }
  reporter.status(line);
}

std::string direct_failure_summary(const PunchStats& stats) {
  if (!stats.attempted) return "not_attempted";
  std::string summary;
  if (!stats.failures.empty()) summary += count_map_summary(stats.failures);
  if (!stats.candidate_failures_by_kind.empty()) {
    if (!summary.empty()) summary += ";";
    summary += "candidate_kinds=" + count_map_summary(stats.candidate_failures_by_kind);
  }
  return summary.empty() ? "unknown" : summary;
}

RouteOutcome make_route_outcome(const std::string& data_path, const std::string& reason, bool direct_attempted,
                                bool lan_upgrade, const PunchStats& stats);

std::optional<Message> recv_relay_control_if_ready(TcpSocket& relay, std::chrono::milliseconds poll_timeout) {
  const int fd = relay.fd();
  if (fd < 0) return std::nullopt;
  const int poll_ms = static_cast<int>(std::max<std::int64_t>(0, poll_timeout.count()));
  if (net_poll(fd, /*want_read=*/true, /*want_write=*/false, poll_ms) <= 0) return std::nullopt;
  return recv_message_timeout(relay, std::chrono::milliseconds(250));
}

RouteSelection relay_selection_from_start(TcpSocket relay, const Message& start, const AdaptivePuncher& puncher,
                                          const RoutePlan& route_plan, bool direct_attempted,
                                          bool explain_direct_failure, ProgressReporter& reporter,
                                          const std::string& result_reason, RouteTiming timing) {
  RouteSelection selection;
  selection.relay = std::move(relay);
  selection.path = RoutePath::Relay;
  selection.allow_lan_upgrade = start.get("reason") != "mismatch";
  selection.punch_stats = punch_stats_from(puncher, false, direct_attempted);
  selection.explain_direct_failure = explain_direct_failure;
  selection.timing = timing;
  report_route_result(reporter, "relay", result_reason, direct_attempted, selection.allow_lan_upgrade);
  report_route_detail(reporter, selection.punch_stats);
  selection.outcome =
      make_route_outcome("relay", result_reason, direct_attempted, selection.allow_lan_upgrade, selection.punch_stats);
  reporter.route_outcome(selection.outcome);
  (void)route_plan;
  return selection;
}

std::optional<TcpSocket> finish_direct_future(std::future<std::optional<TcpSocket>>& future, std::atomic_bool& cancel,
                                              bool suppress_errors) {
  cancel.store(true);
  try {
    auto direct = future.get();
    if (direct) direct->close();
    return direct;
  } catch (const std::exception&) {
    if (!suppress_errors) throw;
  } catch (...) {
    if (!suppress_errors) throw;
  }
  return std::nullopt;
}

std::optional<TcpSocket> collect_direct_future(std::future<std::optional<TcpSocket>>& future,
                                               ProgressReporter& reporter) {
  try {
    return future.get();
  } catch (const std::exception& error) {
    reporter.status(std::string("direct attempt failed: ") + error.what());
  } catch (...) {
    reporter.status("direct attempt failed: unknown error");
  }
  return std::nullopt;
}

RouteOutcome make_route_outcome(const std::string& data_path, const std::string& reason, bool direct_attempted,
                                bool lan_upgrade, const PunchStats& stats) {
  RouteOutcome outcome;
  outcome.control_path = "relay";
  outcome.data_path = data_path;
  outcome.reason = reason;
  outcome.direct_attempted = direct_attempted;
  outcome.lan_upgrade = lan_upgrade;
  if (stats.direct_ok) {
    outcome.direct_candidate_kind = stats.successful_candidate_kind;
    outcome.direct_candidate_priority = stats.successful_candidate_priority;
    outcome.direct_elapsed_ms = stats.successful_elapsed_ms;
  } else {
    outcome.direct_failure_summary = direct_failure_summary(stats);
  }
  return outcome;
}

}  // namespace

RouteSelection select_transfer_route(TcpSocket relay, std::optional<TcpSocket> direct,
                                     const AdaptivePuncher& puncher, const RoutePlan& route_plan,
                                     ProgressReporter& reporter,
                                     std::chrono::milliseconds confirmation_timeout, RouteTiming timing) {
  RouteSelection selection;
  selection.relay = std::move(relay);
  selection.timing = timing;

  if (direct) {
    reporter.status("direct connection established");
    send_message(selection.relay, Message{"direct_ok", {}});
    const auto confirmation_start = std::chrono::steady_clock::now();
    auto direct_choice = recv_message_timeout(selection.relay, confirmation_timeout);
    if (!direct_choice) throw KikoError("relay closed before route confirmation");

    if (direct_choice->type == "relay_start") {
      reporter.status("peer selected relay; using relay path");
      report_route_phase(reporter, RoutePhase::RelayCommitted, "peer selected relay", "peer_selected_relay",
                         /*relay_fallback_ready=*/true);
      direct->close();
      selection.path = RoutePath::Relay;
      selection.allow_lan_upgrade = false;
      selection.punch_stats = punch_stats_from(puncher, false, true);
      selection.timing.relay_commit_ms = elapsed_ms_since(confirmation_start);
      report_route_result(reporter, "relay", "peer_selected_relay", true, selection.allow_lan_upgrade);
      report_route_detail(reporter, selection.punch_stats);
      selection.outcome = make_route_outcome("relay", "peer_selected_relay", true, selection.allow_lan_upgrade,
                                             selection.punch_stats);
      reporter.route_outcome(selection.outcome);
      return selection;
    }
    if (direct_choice->type != "direct_start") {
      throw KikoError("unexpected relay route confirmation: " + direct_choice->type);
    }

    selection.path = RoutePath::Direct;
    selection.direct = std::move(direct);
    selection.punch_stats = punch_stats_from(puncher, true, true);
    report_route_result(reporter, "direct", "confirmed", true, false);
    report_route_detail(reporter, selection.punch_stats);
    selection.outcome = make_route_outcome("direct", "confirmed", true, false, selection.punch_stats);
    reporter.route_outcome(selection.outcome);
    return selection;
  }

  selection.punch_stats = punch_stats_from(puncher, false, !route_plan.skip_direct);
  selection.explain_direct_failure = true;
  const std::string relay_reason = route_plan.skip_direct ? "direct_skipped" : "direct_failed";
  reporter.status(route_plan.skip_direct ? "direct skipped, using relay" : "direct failed, using relay");
  report_route_phase(reporter, RoutePhase::RelayCommitted,
                     route_plan.skip_direct ? "direct skipped; using relay" : "direct failed; using relay",
                     relay_reason, /*relay_fallback_ready=*/true);
  const auto commit_start = std::chrono::steady_clock::now();
  send_message(selection.relay, Message{"relay_ready", {}});
  auto start = recv_message_timeout(selection.relay, confirmation_timeout);
  if (!start || start->type != "relay_start") throw KikoError("relay did not start stream");
  selection.timing.relay_commit_ms = elapsed_ms_since(commit_start);
  selection.path = RoutePath::Relay;
  selection.allow_lan_upgrade = start->get("reason") != "mismatch";
  report_route_result(reporter, "relay", relay_reason, !route_plan.skip_direct, selection.allow_lan_upgrade);
  report_route_detail(reporter, selection.punch_stats);
  selection.outcome =
      make_route_outcome("relay", relay_reason, !route_plan.skip_direct, selection.allow_lan_upgrade,
                         selection.punch_stats);
  reporter.route_outcome(selection.outcome);
  return selection;
}

RouteSelection race_transfer_route(TcpSocket relay, DirectAttemptFn direct_attempt, const AdaptivePuncher& puncher,
                                   const RoutePlan& route_plan, ProgressReporter& reporter,
                                   std::chrono::milliseconds confirmation_timeout, RouteTiming timing) {
  if (route_plan.skip_direct || !direct_attempt) {
    return select_transfer_route(std::move(relay), std::nullopt, puncher, route_plan, reporter, confirmation_timeout,
                                 timing);
  }

  send_message(relay, Message{"relay_standby", {}});
  reporter.status("relay standby; trying direct");
  report_route_phase(reporter, RoutePhase::RelayStandby, "relay fallback is ready", route_plan.reason,
                     /*relay_fallback_ready=*/true);
  report_route_phase(reporter, RoutePhase::DirectProbing, "trying direct with relay fallback ready",
                     route_plan.reason, /*relay_fallback_ready=*/true);

  std::atomic_bool cancel{false};
  const auto direct_probe_start = std::chrono::steady_clock::now();
  auto direct_future = std::async(std::launch::async, [&]() { return direct_attempt(&cancel); });

  while (direct_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    if (auto relay_msg = recv_relay_control_if_ready(relay, std::chrono::milliseconds(25))) {
      if (relay_msg->type == "relay_start") {
        timing.direct_probe_ms = elapsed_ms_since(direct_probe_start);
        const auto commit_start = std::chrono::steady_clock::now();
        reporter.status("relay committed by peer; canceling direct");
        report_route_phase(reporter, RoutePhase::RelayCommitted, "peer selected relay; canceling direct",
                           "peer_selected_relay", /*relay_fallback_ready=*/true);
        (void)finish_direct_future(direct_future, cancel, /*suppress_errors=*/true);
        timing.relay_commit_ms = elapsed_ms_since(commit_start);
        return relay_selection_from_start(std::move(relay), *relay_msg, puncher, route_plan,
                                          /*direct_attempted=*/true, /*explain_direct_failure=*/false, reporter,
                                          "peer_selected_relay", timing);
      }
      if (relay_msg->type == "error") throw KikoError("relay route error: " + relay_msg->get("code", "unknown"));
      if (relay_msg->type == "done") throw KikoError("relay closed before route selection");
      throw KikoError("unexpected relay route message while direct pending: " + relay_msg->type);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  timing.direct_probe_ms = elapsed_ms_since(direct_probe_start);
  auto direct = collect_direct_future(direct_future, reporter);
  if (auto relay_msg = recv_relay_control_if_ready(relay, std::chrono::milliseconds(35))) {
    if (relay_msg->type == "relay_start") {
      if (direct) direct->close();
      timing.relay_commit_ms = 0;
      reporter.status("relay committed before direct confirmation");
      report_route_phase(reporter, RoutePhase::RelayCommitted, "peer selected relay before direct confirmation",
                         "peer_selected_relay", /*relay_fallback_ready=*/true);
      return relay_selection_from_start(std::move(relay), *relay_msg, puncher, route_plan,
                                        /*direct_attempted=*/true, /*explain_direct_failure=*/false, reporter,
                                        "peer_selected_relay", timing);
    }
    if (relay_msg->type == "error") throw KikoError("relay route error: " + relay_msg->get("code", "unknown"));
    if (relay_msg->type == "done") throw KikoError("relay closed before route selection");
    throw KikoError("unexpected relay route message before route confirmation: " + relay_msg->type);
  }

  return select_transfer_route(std::move(relay), std::move(direct), puncher, route_plan, reporter,
                               confirmation_timeout, timing);
}

}  // namespace kiko
