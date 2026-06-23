#pragma once

#include "tui_transfer_spec.hpp"
#include "tui_transfer_view.hpp"

#include <functional>
#include <memory>
#include <thread>

namespace kiko {

class TransferCancellation;

[[nodiscard]] std::thread start_tui_transfer(TuiTransferSpec spec, TuiState& state, std::function<void()> wake,
                                             std::shared_ptr<TransferCancellation> cancellation);

}  // namespace kiko
