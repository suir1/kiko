#pragma once

#include <string_view>

namespace kiko::detail {

[[nodiscard]] bool is_retryable_transfer_error_message(std::string_view message);
[[nodiscard]] int total_transfer_attempts(bool auto_reconnect, int reconnect_attempts);

}  // namespace kiko::detail
