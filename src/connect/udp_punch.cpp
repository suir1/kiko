#include "udp_punch.hpp"

#include "core/cancellation.hpp"
#include "platform/platform.hpp"

#include <algorithm>
#include <limits>
#include <thread>

namespace kiko {
namespace {

constexpr int kMaxUdpPackets = 32;
constexpr const char kPunchPayload[] = "kiko-punch";

bool cancelled(const std::atomic_bool* cancel) { return cancellation_requested(cancel); }

void wait_until_punch_time(std::uint64_t punch_at_ms, const std::atomic_bool* cancel) {
  while (true) {
    if (cancelled(cancel)) return;
    const auto now = now_ms();
    if (now >= punch_at_ms) return;
    if (punch_at_ms - now > 2000) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

bool send_udp_packet(const Endpoint& target, const void* data, std::size_t size) {
  if (size > static_cast<std::size_t>(std::numeric_limits<int>::max())) return false;

  net_startup();
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* res = nullptr;
  if (getaddrinfo(target.host.c_str(), std::to_string(target.port).c_str(), &hints, &res) != 0 || !res) {
    return false;
  }

  NativeSocketHandle fd = kInvalidNativeSocket;
  const addrinfo* selected = nullptr;
  for (auto* ai = res; ai != nullptr; ai = ai->ai_next) {
    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (net_socket_valid(fd)) {
      selected = ai;
      break;
    }
  }
  if (!net_socket_valid(fd) || !selected) {
    freeaddrinfo(res);
    return false;
  }

  const auto* send_buf = reinterpret_cast<const char*>(data);
  const auto send_size = static_cast<int>(size);
  const auto sent = sendto(fd, send_buf, send_size, 0, selected->ai_addr,
                           static_cast<socklen_t>(selected->ai_addrlen));
  net_close(fd);
  freeaddrinfo(res);
  return sent == send_size;
}

}  // namespace

void udp_punch_burst(const UdpPunchParams& params) {
  if (params.peer_wan.host.empty() || params.peer_wan.port == 0) return;
  const auto punch_at = parse_u64_strict(params.token);
  if (!punch_at) return;

  wait_until_punch_time(*punch_at, params.cancel);
  if (cancelled(params.cancel)) return;

  const auto interval_ms = params.window.count() > 0
                               ? std::max<std::int64_t>(1, params.window.count() / kMaxUdpPackets)
                               : 1;
  for (int i = 0; i < kMaxUdpPackets; ++i) {
    if (cancelled(params.cancel)) return;
    (void)send_udp_packet(params.peer_wan, kPunchPayload, sizeof(kPunchPayload) - 1);
    if (i + 1 < kMaxUdpPackets) std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
  }
}

std::optional<TcpSocket> try_udp_assisted_direct(Role role, TcpListener& listener, const Endpoint& peer_wan,
                                                 const std::string& punch_token, PunchPlan plan,
                                                 AdaptivePuncher& puncher, const std::string& room,
                                                 const ConnectOptions& connect_options,
                                                 const std::atomic_bool* cancel) {
  if (plan.total_timeout.count() <= 0) return std::nullopt;

  std::thread udp_thread;
  if (!punch_token.empty() && !peer_wan.host.empty() && peer_wan.port > 0) {
    UdpPunchParams params;
    params.role = role;
    params.peer_wan = peer_wan;
    params.token = punch_token;
    params.window = std::chrono::milliseconds(400);
    params.cancel = cancel;
    udp_thread = std::thread([params]() { udp_punch_burst(params); });
  }

  auto result = try_direct_with_plan(role, listener, plan, puncher, room, connect_options, punch_token, cancel);
  if (udp_thread.joinable()) udp_thread.join();
  return result;
}

}  // namespace kiko
