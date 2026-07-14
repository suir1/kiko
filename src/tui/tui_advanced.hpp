#pragma once

#include "transfer/transfer.hpp"
#include "platform/user_config.hpp"

#include <optional>
#include <string>

namespace kiko {

struct TuiNetworkOptions {
  int preset = 0;
  bool advanced_open = false;
  bool lan_discover = true;
  bool only_local = false;
  bool disable_local = false;
  bool no_direct = false;
  bool udp_probe = false;
  bool auto_connections = false;
  int connections = 4;
  bool use_gitignore = true;
  bool avoid_vpn = false;
  std::string manual_ip;
  std::string bind_interface;
  std::string proxy_url;
};

[[nodiscard]] const char* network_preset_label(int preset);

void apply_network_preset(int preset, TuiNetworkOptions& options);

void load_network_options(const UserConfig& config, TuiNetworkOptions& options);
void save_network_options(UserConfig& config, const TuiNetworkOptions& options);

void apply_network_options_to_peer(PeerConnectionOptions& config, const TuiNetworkOptions& options);
void apply_network_options_to_send(SendConfig& config, const TuiNetworkOptions& options);

[[nodiscard]] std::optional<std::string> validate_network_options(const TuiNetworkOptions& options, int mode);

}  // namespace kiko
