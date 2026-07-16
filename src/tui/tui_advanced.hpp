#pragma once

#include "transfer/transfer.hpp"
#include "platform/user_config.hpp"

#include <optional>
#include <string>

namespace kiko {

using TuiNetworkOptions = NetworkPreferences;

[[nodiscard]] const char* network_preset_label(int preset);

void apply_network_preset(int preset, TuiNetworkOptions& options);

void apply_network_options_to_peer(PeerConnectionOptions& config, const TuiNetworkOptions& options);
void apply_network_options_to_send(SendConfig& config, const TuiNetworkOptions& options);

[[nodiscard]] std::optional<std::string> validate_network_options(const TuiNetworkOptions& options);

}  // namespace kiko
