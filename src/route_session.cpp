#include "route_session.hpp"

#include "protocol.hpp"

#include <sstream>

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
  return selection;
}

}  // namespace kiko
