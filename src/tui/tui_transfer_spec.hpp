#pragma once

#include "core/common.hpp"
#include "tui_advanced.hpp"

#include <optional>
#include <string>

namespace kiko {

struct TuiTransferSpec {
  int mode = 0;
  std::string path;
  std::string code;
  std::string output_dir;
  Endpoint relay;
  std::optional<std::string> relay_pass;
  NetworkPreferences network;
};

}  // namespace kiko
