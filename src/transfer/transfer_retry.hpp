#pragma once

#include <string_view>

namespace kiko::detail {

[[nodiscard]] bool is_retryable_transfer_error_message(std::string_view message);

}  // namespace kiko::detail
