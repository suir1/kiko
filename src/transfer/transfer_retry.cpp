#include "transfer_retry.hpp"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string>

namespace kiko::detail {

namespace {

std::string lowercase(std::string_view text) {
  std::string out(text);
  for (char& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return out;
}

bool contains_any(std::string_view text, std::initializer_list<std::string_view> needles) {
  for (const auto needle : needles) {
    if (text.find(needle) != std::string_view::npos) return true;
  }
  return false;
}

}  // namespace

bool is_retryable_transfer_error_message(std::string_view message) {
  const auto text = lowercase(message);
  if (contains_any(text,
                   {
                       "bad_password",
                       "bad password",
                       "invalid_hello",
                       "invalid hello",
                       "rendezvous peer",
                       "pairing code",
                       "control message field",
                       "invalid relay",
                       "connection count exceeds maximum",
                       "not a file or directory",
                       "permission denied",
                       "failed to open input file",
                       "failed to open output file",
                       "failed to open received file",
                       "failed to create symlink",
                       "failed to finalize file",
                       "write failed during mux receive",
                       "integrity check failed",
                       "sha256 mismatch",
                       "hash mismatch",
                       "invalid resume ack",
                       "received more data than declared",
                       "path traversal",
                       "unsafe symlink",
                       "transfer manifest",
                       "receive plan",
                       "file header",
                   })) {
    return false;
  }

  return contains_any(text,
                      {
                          "connection reset",
                          "connection refused",
                          "connection timed",
                          "connection timeout",
                          "connection aborted",
                          "connection closed",
                          "connection lost",
                          "connection interrupted",
                          "connection attempt failed",
                          "connection was aborted",
                          "forcibly closed",
                          "actively refused",
                          "operation aborted",
                          "operation has been aborted",
                          "network name is no longer available",
                          "broken pipe",
                          "timed out",
                          "timeout",
                          "ended unexpectedly",
                          "closed early",
                          "failed to connect",
                          "connect failed",
                          "relay did not",
                          "relay route error",
                          "recv failed",
                          "send failed",
                          "data channel closed",
                          "mux receive failed: data channel closed early",
                          "expected mux chunk end",
                          "transfer stream ended unexpectedly",
                          "expected transfer ack",
                          "interrupted",
                      });
}

bool is_retryable_transfer_error(const std::exception& error) {
  return is_retryable_transfer_error_message(error.what());
}

int total_transfer_attempts(bool auto_reconnect, int reconnect_attempts) {
  if (!auto_reconnect) return 1;
  return std::max(1, reconnect_attempts);
}

}  // namespace kiko::detail
