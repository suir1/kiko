#include "outbound_policy.hpp"

#include "network_probe.hpp"
#include "relay/relay_race.hpp"

#include <chrono>

namespace kiko {
namespace {

constexpr auto kAutoProbeTimeout = std::chrono::milliseconds(900);
constexpr std::int64_t kRttWinMarginMs = 5;

OutboundProbe probe_path(const Endpoint& relay, const std::string& path, const ConnectOptions& options) {
  OutboundProbe probe;
  probe.path = path;
  probe.bind_interface = options.bind_interface;
  probe.rtt_ms = probe_relay_rtt_ms(relay, options, kAutoProbeTimeout);
  probe.pong_ok = probe.rtt_ms >= 0;
  return probe;
}

bool materially_faster(std::int64_t candidate_ms, std::int64_t current_ms) {
  return candidate_ms >= 0 && current_ms >= 0 && candidate_ms + kRttWinMarginMs < current_ms;
}

bool history_prefers_physical(const std::optional<OutboundHistory>& history) {
  if (!history || history->path != "physical" || history->bind_interface.empty()) return false;
  if (history->reason == "physical_lower_rtt" || history->reason == "default_failed_physical_ok" ||
      history->reason == "avoid_vpn" || history->reason == "profile_physical_history") {
    return true;
  }
  const auto default_rtt = history->rtt_by_path.find("default");
  const auto physical_rtt = history->rtt_by_path.find("physical");
  return default_rtt != history->rtt_by_path.end() && physical_rtt != history->rtt_by_path.end() &&
         materially_faster(physical_rtt->second, default_rtt->second);
}

}  // namespace

bool relay_target_is_local(const Endpoint& relay) {
  return is_loopback_host(relay.host);
}

OutboundSelection select_outbound_for_relay(const Endpoint& relay, const std::optional<ProxyConfig>& proxy,
                                            const std::string& bind_interface, bool avoid_vpn,
                                            const std::optional<OutboundHistory>& history) {
  return select_outbound_for_relay(relay, proxy, bind_interface, avoid_vpn, history,
                                   collect_network_interface_inventory());
}

OutboundSelection select_outbound_for_relay(const Endpoint& relay, const std::optional<ProxyConfig>& proxy,
                                            const std::string& bind_interface, bool avoid_vpn,
                                            const std::optional<OutboundHistory>& history,
                                            const NetworkInterfaceInventory& interfaces) {
  OutboundSelection selection;
  selection.connect_options.proxy = proxy;

  if (!bind_interface.empty()) {
    selection.connect_options.bind_interface = bind_interface;
    selection.chosen_path = "forced";
    selection.reason = "user_forced_interface";
    return selection;
  }

  if (relay_target_is_local(relay)) {
    selection.reason = "local_relay";
    return selection;
  }

  if (proxy) {
    selection.reason = "proxy_default";
    return selection;
  }

  const auto physical = interfaces.preferred_physical_interface();

  if (avoid_vpn) {
    if (physical) {
      selection.connect_options.bind_interface = *physical;
      selection.chosen_path = "physical";
      selection.reason = "avoid_vpn";
    } else {
      selection.reason = "avoid_vpn_no_physical_interface";
    }
    return selection;
  }

  if (history_prefers_physical(history)) {
    selection.connect_options.bind_interface = history->bind_interface;
    selection.chosen_path = "physical";
    selection.reason = "profile_physical_history";
    return selection;
  }

  if (!interfaces.vpn_detected() || !physical) {
    selection.reason = "default";
    return selection;
  }

  ConnectOptions default_options;
  default_options.proxy = proxy;
  auto default_probe = probe_path(relay, "default", default_options);
  selection.probes.push_back(default_probe);

  ConnectOptions physical_options;
  physical_options.proxy = proxy;
  physical_options.bind_interface = *physical;
  auto physical_probe = probe_path(relay, "physical", physical_options);
  selection.probes.push_back(physical_probe);

  if (physical_probe.pong_ok && !default_probe.pong_ok) {
    selection.connect_options = physical_options;
    selection.chosen_path = "physical";
    selection.reason = "default_failed_physical_ok";
    return selection;
  }
  if (physical_probe.pong_ok && default_probe.pong_ok &&
      materially_faster(physical_probe.rtt_ms, default_probe.rtt_ms)) {
    selection.connect_options = physical_options;
    selection.chosen_path = "physical";
    selection.reason = "physical_lower_rtt";
    return selection;
  }

  selection.connect_options = default_options;
  selection.chosen_path = "default";
  if (default_probe.pong_ok) {
    selection.reason = physical_probe.pong_ok ? "default_lower_or_similar_rtt" : "physical_failed_default_ok";
  } else {
    selection.reason = "both_paths_failed";
  }
  return selection;
}

}  // namespace kiko
