#pragma once

#include "core/common.hpp"
#include "platform/user_config.hpp"

#include <optional>
#include <string>

namespace kiko {

struct WebOptions {
  Endpoint listen{"127.0.0.1", 0};
  Endpoint relay;
  std::optional<std::string> relay_pass;
  bool open_browser = true;
  UserConfig user_config;
};

[[nodiscard]] std::string generate_web_token();

int run_web_console(const WebOptions& options);

}  // namespace kiko
