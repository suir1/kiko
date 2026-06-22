#pragma once

#include <exception>
#include <string_view>

namespace kiko::detail {

[[nodiscard]] bool is_retryable_transfer_error_message(std::string_view message);
[[nodiscard]] bool is_retryable_transfer_error(const std::exception& error);
[[nodiscard]] int total_transfer_attempts(bool auto_reconnect, int reconnect_attempts);

}  // namespace kiko::detail
