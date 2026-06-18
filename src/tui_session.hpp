#pragma once

#include "tui_transfer_spec.hpp"
#include "tui_transfer_view.hpp"

#include <functional>
#include <thread>

namespace kiko {

[[nodiscard]] std::thread start_tui_transfer(TuiTransferSpec spec, TuiState& state, std::function<void()> wake);

}  // namespace kiko
