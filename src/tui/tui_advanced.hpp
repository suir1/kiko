#pragma once

#include "transfer/transfer.hpp"
#include "platform/user_config.hpp"

#include <optional>
#include <string>

namespace kiko {

[[nodiscard]] const char* network_preset_label(int preset);

void apply_network_preset(int preset, NetworkPreferences& options);

void apply_network_options_to_peer(PeerConnectionOptions& config, const NetworkPreferences& options);
void apply_network_options_to_send(SendConfig& config, const NetworkPreferences& options);

[[nodiscard]] std::optional<std::string> validate_network_options(const NetworkPreferences& options);

}  // namespace kiko
