#include "diagnostics/outbound_policy.hpp"

#include <cassert>
#include <iostream>

int main() {
  using namespace kiko;

  {
    assert(relay_target_is_local(Endpoint{"127.0.0.1", 9000}));
    assert(relay_target_is_local(Endpoint{"::1", 9000}));
    assert(relay_target_is_local(Endpoint{"localhost", 9000}));
    assert(!relay_target_is_local(Endpoint{"203.0.113.10", 9000}));
  }

  {
    const auto selection =
        select_outbound_for_relay(Endpoint{"203.0.113.10", 9000}, std::nullopt, "en9", false);
    assert(selection.chosen_path == "forced");
    assert(selection.reason == "user_forced_interface");
    assert(selection.connect_options.bind_interface == "en9");
    assert(selection.probes.empty());
  }

  {
    const auto selection = select_outbound_for_relay(Endpoint{"127.0.0.1", 9000}, std::nullopt, "", false);
    assert(selection.chosen_path == "default");
    assert(selection.reason == "local_relay");
    assert(selection.connect_options.bind_interface.empty());
    assert(selection.probes.empty());
  }

  {
    ProxyConfig proxy;
    proxy.type = ProxyType::Socks5;
    proxy.endpoint = Endpoint{"127.0.0.1", 1080};
    const auto selection = select_outbound_for_relay(Endpoint{"relay.example", 9000}, proxy, "", false);
    assert(selection.chosen_path == "default");
    assert(selection.reason == "proxy_default");
    assert(selection.connect_options.proxy.has_value());
    assert(selection.connect_options.proxy->endpoint.host == "127.0.0.1");
    assert(selection.connect_options.proxy->endpoint.port == 1080);
    assert(selection.connect_options.bind_interface.empty());
    assert(selection.probes.empty());
  }

  {
    OutboundHistory history;
    history.path = "physical";
    history.bind_interface = "en9";
    history.reason = "physical_lower_rtt";
    history.rtt_by_path["default"] = 90;
    history.rtt_by_path["physical"] = 40;
    const auto selection =
        select_outbound_for_relay(Endpoint{"relay.example", 9000}, std::nullopt, "", false, history);
    assert(selection.chosen_path == "physical");
    assert(selection.reason == "profile_physical_history");
    assert(selection.connect_options.bind_interface == "en9");
    assert(selection.probes.empty());
  }

  {
    OutboundHistory history;
    history.path = "physical";
    history.bind_interface = "en9";
    history.reason = "physical_lower_rtt";
    ProxyConfig proxy;
    proxy.type = ProxyType::Socks5;
    proxy.endpoint = Endpoint{"127.0.0.1", 1080};
    const auto selection = select_outbound_for_relay(Endpoint{"relay.example", 9000}, proxy, "", false, history);
    assert(selection.chosen_path == "default");
    assert(selection.reason == "proxy_default");
    assert(selection.connect_options.bind_interface.empty());
  }

  std::cout << "PASS: outbound policy decisions\n";
  return 0;
}
