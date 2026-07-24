#pragma once

#include "tui_transfer_config.hpp"
#include "tui_transfer_view.hpp"

#include <functional>
#include <memory>
#include <thread>

namespace kiko {

class TransferCancellation;

using TuiTask =
    std::function<void(ProgressReporter&, const std::shared_ptr<TransferCancellation>&)>;

[[nodiscard]] std::thread start_tui_task(TuiTask task, TuiState& state, std::function<void()> wake,
                                         std::shared_ptr<TransferCancellation> cancellation);

[[nodiscard]] std::thread start_tui_transfer(TuiTransferConfig config, TuiState& state, std::function<void()> wake,
                                             std::shared_ptr<TransferCancellation> cancellation);

}  // namespace kiko
