#pragma once

#include "core/common.hpp"

#include <optional>
#include <string>

namespace kiko {

void show_doctor_modal(const Endpoint& relay, const std::optional<std::string>& relay_pass, bool udp_probe = false);

}  // namespace kiko
