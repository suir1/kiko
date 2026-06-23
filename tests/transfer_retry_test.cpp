#include "common.hpp"
#include "transfer/transfer_retry.hpp"

#include <exception>
#include <iostream>
#include <string_view>
#include <vector>

using namespace kiko;
using namespace kiko::detail;

namespace {

struct Case {
  std::string_view message;
  bool retryable;
};

bool check_case(const Case& test_case) {
  const bool got = is_retryable_transfer_error_message(test_case.message);
  if (got == test_case.retryable) return true;
  std::cerr << "FAIL: retry classifier for '" << test_case.message << "' returned "
            << (got ? "retryable" : "fatal") << ", expected " << (test_case.retryable ? "retryable" : "fatal")
            << "\n";
  return false;
}

}  // namespace

int main() {
  const std::vector<Case> cases = {
      {"connection reset by peer", true},
      {"broken pipe while sending data", true},
      {"relay route error: data channel closed", true},
      {"mux receive failed: data channel closed early", true},
      {"expected mux chunk end", true},
      {"transfer stream ended unexpectedly", true},
      {"expected transfer ack", true},
      {"recv failed: timed out", true},
      {"send failed: connection closed", true},
      {"simulated connection interrupted", true},
      {"write: An existing connection was forcibly closed by the remote host.", true},
      {"write: An established connection was aborted by the software in your host machine.", true},
      {"connect: No connection could be made because the target machine actively refused it.", true},
      {"write: The network name is no longer available.", true},
      {"read: The I/O operation has been aborted because of either a thread exit or an application request.", true},
      {"bad_password", false},
      {"invalid_hello from relay", false},
      {"invalid relay endpoint", false},
      {"pairing code is required", false},
      {"connection count exceeds maximum 32", false},
      {"failed to open input file: missing.bin", false},
      {"permission denied opening output file", false},
      {"write failed during mux receive", false},
      {"mux receive failed: write failed during mux receive", false},
      {"integrity check failed for payload.bin", false},
      {"sha256 mismatch after transfer", false},
      {"invalid resume ack for payload.bin", false},
      {"received more data than declared for payload.bin", false},
      {"refusing path traversal in transfer: ../evil.bin", false},
      {"refusing unsafe symlink target for link: ../target", false},
      {"transfer manifest contains duplicate path: a.txt", false},
      {"receive plan target collision for a.txt", false},
      {"file header size does not match manifest for a.txt", false},
  };

  for (const auto& test_case : cases) {
    if (!check_case(test_case)) return 1;
  }

  if (total_transfer_attempts(false, 5) != 1) {
    std::cerr << "FAIL: disabled reconnect should use one attempt\n";
    return 1;
  }
  if (total_transfer_attempts(true, 0) != 1 || total_transfer_attempts(true, 3) != 3) {
    std::cerr << "FAIL: reconnect attempt normalization mismatch\n";
    return 1;
  }

  KikoError reset("connection reset by peer");
  KikoError hash("integrity check failed for payload.bin");
  if (!is_retryable_transfer_error(reset) || is_retryable_transfer_error(hash)) {
    std::cerr << "FAIL: exception overload mismatch\n";
    return 1;
  }

  std::cout << "PASS: transfer retry classifier\n";
  return 0;
}
