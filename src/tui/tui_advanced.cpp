#include "tui_advanced.hpp"

#include "core/proxy.hpp"

namespace kiko {

const char* network_preset_label(int preset) {
  switch (preset) {
    case 1:
      return "同 Wi-Fi 加速";
    case 2:
      return "公司 / 仅 relay";
    case 3:
      return "调试连通";
    default:
      return "公网互传（默认）";
  }
}

void apply_network_preset(int preset, TuiNetworkOptions& options) {
  options.preset = preset;
  options.lan_discover = true;
  options.only_local = false;
  options.disable_local = false;
  options.no_direct = false;
  options.udp_probe = false;
  options.auto_connections = false;
  options.connections = 4;
  options.use_gitignore = true;
  options.avoid_vpn = false;

  switch (preset) {
    case 1:
      options.auto_connections = true;
      break;
    case 2:
      options.lan_discover = false;
      options.no_direct = true;
      break;
    case 3:
      options.udp_probe = true;
      break;
    default:
      break;
  }
}

void apply_network_options_to_peer(PeerConnectionOptions& config, const TuiNetworkOptions& options) {
  config.lan_discover = options.lan_discover;
  config.only_local = options.only_local;
  config.disable_local = options.disable_local;
  config.no_direct = options.no_direct;
  config.udp_probe = options.udp_probe;
  config.avoid_vpn = options.avoid_vpn;
  config.bind_interface = options.bind_interface;
  config.manual_ip = options.manual_ip.empty() ? std::nullopt : std::optional<std::string>(options.manual_ip);
  config.proxy = std::nullopt;
  if (!options.proxy_url.empty()) config.proxy = parse_proxy_url(options.proxy_url);
}

void apply_network_options_to_send(SendConfig& config, const TuiNetworkOptions& options) {
  apply_network_options_to_peer(config, options);
  config.auto_connections = options.auto_connections;
  config.connections = options.connections;
  config.use_gitignore = options.use_gitignore;
}

std::optional<std::string> validate_network_options(const TuiNetworkOptions& options, int mode) {
  if (options.only_local && options.disable_local) {
    return "only local and disable local cannot both be enabled";
  }
  if (options.connections < 1 || options.connections > 64) {
    return "connections must be between 1 and 64";
  }
  if (!options.proxy_url.empty()) {
    try {
      (void)parse_proxy_url(options.proxy_url);
    } catch (const std::exception& e) {
      return std::string("invalid proxy URL: ") + e.what();
    }
  }
  (void)mode;
  return std::nullopt;
}

}  // namespace kiko
