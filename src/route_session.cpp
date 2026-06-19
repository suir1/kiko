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
                                          const std::string& result_reason) {
  RouteSelection selection;
  selection.relay = std::move(relay);
  selection.path = RoutePath::Relay;
  selection.allow_lan_upgrade = start.get("reason") != "mismatch";
  selection.punch_stats = punch_stats_from(puncher, false, direct_attempted);
  selection.explain_direct_failure = explain_direct_failure;
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
  outcome.fallback_ready = data_path == "direct";
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
                                     std::chrono::milliseconds confirmation_timeout) {
  RouteSelection selection;
  selection.relay = std::move(relay);

  if (direct) {
    reporter.status("direct connection established");
    send_message(selection.relay, Message{"direct_ok", {}});
    auto direct_choice = recv_message_timeout(selection.relay, confirmation_timeout);
    if (!direct_choice) throw KikoError("relay closed before route confirmation");

    if (direct_choice->type == "relay_start") {
      reporter.status("peer selected relay; using relay path");
      direct->close();
      selection.path = RoutePath::Relay;
      selection.allow_lan_upgrade = false;
      selection.punch_stats = punch_stats_from(puncher, false, true);
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
  reporter.status(route_plan.skip_direct ? "direct skipped, using relay" : "direct failed, using relay");
  send_message(selection.relay, Message{"relay_ready", {}});
  auto start = recv_message_timeout(selection.relay, confirmation_timeout);
  if (!start || start->type != "relay_start") throw KikoError("relay did not start stream");
  selection.path = RoutePath::Relay;
  selection.allow_lan_upgrade = start->get("reason") != "mismatch";
  report_route_result(reporter, "relay", route_plan.skip_direct ? "direct_skipped" : "direct_failed",
                      !route_plan.skip_direct, selection.allow_lan_upgrade);
  report_route_detail(reporter, selection.punch_stats);
  selection.outcome = make_route_outcome("relay", route_plan.skip_direct ? "direct_skipped" : "direct_failed",
                                         !route_plan.skip_direct, selection.allow_lan_upgrade, selection.punch_stats);
  reporter.route_outcome(selection.outcome);
  return selection;
}

RouteSelection race_transfer_route(TcpSocket relay, DirectAttemptFn direct_attempt, const AdaptivePuncher& puncher,
                                   const RoutePlan& route_plan, ProgressReporter& reporter,
                                   std::chrono::milliseconds confirmation_timeout) {
  if (route_plan.skip_direct || !direct_attempt) {
    return select_transfer_route(std::move(relay), std::nullopt, puncher, route_plan, reporter, confirmation_timeout);
  }

  send_message(relay, Message{"relay_standby", {}});
  reporter.status("relay standby; trying direct");

  std::atomic_bool cancel{false};
  auto direct_future = std::async(std::launch::async, [&]() { return direct_attempt(&cancel); });

  while (direct_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    if (auto relay_msg = recv_relay_control_if_ready(relay, std::chrono::milliseconds(25))) {
      if (relay_msg->type == "relay_start") {
        reporter.status("relay committed by peer; canceling direct");
        (void)finish_direct_future(direct_future, cancel, /*suppress_errors=*/true);
        return relay_selection_from_start(std::move(relay), *relay_msg, puncher, route_plan,
                                          /*direct_attempted=*/true, /*explain_direct_failure=*/false, reporter,
                                          "peer_selected_relay");
      }
      if (relay_msg->type == "error") throw KikoError("relay route error: " + relay_msg->get("code", "unknown"));
      if (relay_msg->type == "done") throw KikoError("relay closed before route selection");
      throw KikoError("unexpected relay route message while direct pending: " + relay_msg->type);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  auto direct = collect_direct_future(direct_future, reporter);
  if (auto relay_msg = recv_relay_control_if_ready(relay, std::chrono::milliseconds(35))) {
    if (relay_msg->type == "relay_start") {
      if (direct) direct->close();
      reporter.status("relay committed before direct confirmation");
      return relay_selection_from_start(std::move(relay), *relay_msg, puncher, route_plan,
                                        /*direct_attempted=*/true, /*explain_direct_failure=*/false, reporter,
                                        "peer_selected_relay");
    }
    if (relay_msg->type == "error") throw KikoError("relay route error: " + relay_msg->get("code", "unknown"));
    if (relay_msg->type == "done") throw KikoError("relay closed before route selection");
    throw KikoError("unexpected relay route message before route confirmation: " + relay_msg->type);
  }

  return select_transfer_route(std::move(relay), std::move(direct), puncher, route_plan, reporter,
                               confirmation_timeout);
}

}  // namespace kiko
