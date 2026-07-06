#include "encrypted_session.hpp"

#include "core/cancellation.hpp"
#include "core/pake.hpp"

#include <chrono>

namespace kiko {
namespace {

int elapsed_ms_since(std::chrono::steady_clock::time_point start) {
  const auto elapsed = std::chrono::steady_clock::now() - start;
  return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

}  // namespace

EncryptedSession secure_encrypted_session(TcpSocket channel, Role role, const std::string& code,
                                          const std::string& route, RouteTiming timing, ProgressReporter& reporter,
                                          TransferCancellation* cancellation) {
  if (cancellation) cancellation->track(channel);
  reporter.route_phase(RoutePhase::Securing,
                       RoutePhaseDetail{"securing " + route + " channel", route, route == "relay"});
  const auto securing_start = std::chrono::steady_clock::now();
  auto key = perform_handshake(channel, role, code);
  timing.securing_ms = elapsed_ms_since(securing_start);
  reporter.route_timing(timing);
  reporter.handshake_ok();
  return EncryptedSession{std::move(channel), std::move(key), timing};
}

}  // namespace kiko
