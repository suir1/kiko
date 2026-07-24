#pragma once

#include "transfer/transfer.hpp"

#include <variant>

namespace kiko {

using TuiTransferConfig = std::variant<SendConfig, RecvConfig>;

}  // namespace kiko
