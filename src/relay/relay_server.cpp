#include "relay_server.hpp"

#include "core/adaptive.hpp"
#include "core/common.hpp"
#include "core/config.hpp"
#include "platform/platform.hpp"
#include "core/protocol.hpp"
#include "relay_protocol.hpp"
#include "relay_room_state.hpp"
#include "relay_route_state.hpp"
#include "core/socket.hpp"
#include <array>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace kiko {
namespace {

constexpr auto kControlReadTimeout = std::chrono::seconds(5);
constexpr auto kRouteChoiceTimeout = std::chrono::seconds(15);
constexpr auto kRoutePollSlice = std::chrono::milliseconds(25);
constexpr auto kRouteFrameReadTimeout = std::chrono::milliseconds(250);
constexpr std::size_t kMaxRelayControlFrameBytes = 64 * 1024;

struct RouteChoiceRead {
  enum class Kind { None, Message, Closed };
  Kind kind = Kind::None;
  Message message;
};

RouteChoiceRead recv_route_choice_if_ready(TcpSocket& socket, const std::atomic_bool* cancel) {
  const auto fd = socket.native_handle();
  if (!net_socket_valid(fd)) return {RouteChoiceRead::Kind::Closed, {}};
  const int poll = net_poll(fd, /*want_read=*/true, /*want_write=*/false, 0);
  if (poll == 0) return {};
  if (poll < 0) return {RouteChoiceRead::Kind::Closed, {}};
  auto message = recv_message_timeout(socket, kRouteFrameReadTimeout, cancel, kMaxRelayControlFrameBytes);
  if (!message) return {RouteChoiceRead::Kind::Closed, {}};
  return {RouteChoiceRead::Kind::Message, std::move(*message)};
}

RelayRouteDecision wait_route_decision(TcpSocket& first, TcpSocket& second, const std::atomic_bool* cancel) {
  RelayRouteChoice first_choice = RelayRouteChoice::Waiting;
  RelayRouteChoice second_choice = RelayRouteChoice::Waiting;
  const auto deadline = std::chrono::steady_clock::now() + kRouteChoiceTimeout;

  while (std::chrono::steady_clock::now() < deadline) {
    if (cancel && cancel->load()) return {RelayRouteDecision::Kind::Invalid, {}};
    const auto first_read = recv_route_choice_if_ready(first, cancel);
    if (first_read.kind == RouteChoiceRead::Kind::Message) {
      merge_relay_route_choice(first_choice, relay_route_choice_from(first_read.message));
    } else if (first_read.kind == RouteChoiceRead::Kind::Closed) {
      merge_relay_route_choice(first_choice, RelayRouteChoice::Invalid);
    }
    const auto second_read = recv_route_choice_if_ready(second, cancel);
    if (second_read.kind == RouteChoiceRead::Kind::Message) {
      merge_relay_route_choice(second_choice, relay_route_choice_from(second_read.message));
    } else if (second_read.kind == RouteChoiceRead::Kind::Closed) {
      merge_relay_route_choice(second_choice, RelayRouteChoice::Invalid);
    }

    auto decision = relay_route_decision_for(first_choice, second_choice, /*deadline_expired=*/false);
    if (decision.kind != RelayRouteDecision::Kind::Pending) return decision;
    std::this_thread::sleep_for(kRoutePollSlice);
  }

  return relay_route_decision_for(first_choice, second_choice, /*deadline_expired=*/true);
}

class RelayStateImpl {
 public:
  explicit RelayStateImpl(RelayServerConfig config) : config_(std::move(config)), rooms_(config_) { start_cleanup(); }

  ~RelayStateImpl() { stop_cleanup(); }

  RelayServerConfig config_;
  RelayRoomState rooms_;
  std::atomic<bool> cleanup_stop_{false};
  std::thread cleanup_thread_;

  void log_client_error(const std::string& error) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    const auto now = std::chrono::steady_clock::now();
    if (now - log_window_started_ >= std::chrono::minutes(1)) {
      log_window_started_ = now;
      logged_client_errors_ = 0;
    }
    if (logged_client_errors_ < 10) {
      std::cerr << "relay client error: " << error << "\n";
    } else if (logged_client_errors_ == 10) {
      std::cerr << "relay client errors suppressed for the remainder of this minute\n";
    }
    ++logged_client_errors_;
  }

 private:
  std::mutex log_mutex_;
  std::chrono::steady_clock::time_point log_window_started_{std::chrono::steady_clock::now()};
  std::size_t logged_client_errors_ = 0;
  std::mutex cleanup_mutex_;
  std::condition_variable cleanup_wakeup_;

  void start_cleanup() {
    cleanup_thread_ = std::thread([this]() {
      while (!cleanup_stop_.load()) {
        const auto interval = config_.cleanup_interval.count() > 0 ? config_.cleanup_interval
                                                                   : std::chrono::seconds(60);
        {
          std::unique_lock<std::mutex> lock(cleanup_mutex_);
          if (cleanup_wakeup_.wait_for(lock, interval, [this] { return cleanup_stop_.load(); })) break;
        }
        rooms_.purge_stale_waiting();
      }
    });
  }

  void stop_cleanup() {
    cleanup_stop_.store(true);
    cleanup_wakeup_.notify_all();
    if (cleanup_thread_.joinable()) cleanup_thread_.join();
  }

};

Message peer_message_for(const RelayWaitingPeer& self, const RelayWaitingPeer& peer,
                         const std::string& punch_token) {
  RelayPeerInfo info;
  info.peer_public = peer.public_endpoint;
  info.peer_listen = peer.listen_endpoint;
  info.peer_local_candidates = peer.local_candidates;
  info.peer_no_direct = peer.no_direct;
  info.self_public = self.public_endpoint;
  info.punch_token = punch_token;
  info.route_commit_v2 = true;
  info.file_count = peer.file_count;
  info.total_size = peer.total_size;
  info.conn_count = peer.conn_count;
  return encode_relay_peer_info(info);
}

void send_peer_messages(RelayWaitingPeer& a, RelayWaitingPeer& b) {
  const auto punch_token = std::to_string(now_ms() + 250);
  send_message(a.socket, peer_message_for(a, b, punch_token));
  send_message(b.socket, peer_message_for(b, a, punch_token));
}

void handle_punch_probe(TcpSocket& socket, const Message& probe, const std::shared_ptr<RelayStateImpl>& state) {
  if (!relay_password_ok(state->config_, probe)) {
    send_message(socket, Message{"error", {{"code", "bad_password"}}});
    return;
  }
  const auto peer_addr = socket.peer_endpoint();
  send_message(socket, Message{"punch_observed",
                               {{"public_host", peer_addr.host}, {"public_port", std::to_string(peer_addr.port)}}});
}

void native_shutdown_both(NativeSocketHandle fd) {
  if (!net_socket_valid(fd)) return;
#ifdef _WIN32
  shutdown(fd, SD_BOTH);
#else
  ::shutdown(fd, SHUT_RDWR);
#endif
}

void disable_native_sigpipe(NativeSocketHandle fd) {
#if !defined(_WIN32) && defined(SO_NOSIGPIPE)
  int enabled = 1;
  (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#else
  (void)fd;
#endif
}

bool native_send_all(NativeSocketHandle fd, const char* data, std::size_t size) {
  std::size_t sent = 0;
  while (sent < size) {
#ifdef _WIN32
    const auto chunk = std::min(size - sent, static_cast<std::size_t>(std::numeric_limits<int>::max()));
    const int rc = send(fd, data + sent, static_cast<int>(chunk), 0);
#else
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    const ssize_t rc = send(fd, data + sent, size - sent, flags);
#endif
    if (rc > 0) {
      sent += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc == 0) return false;
    const int err = net_last_error();
    if (err == kErrIntr) continue;
    if (err == kErrWouldBlock) {
      if (net_poll(fd, /*want_read=*/false, /*want_write=*/true, 250) <= 0) return false;
      continue;
    }
    return false;
  }
  return true;
}

std::optional<std::size_t> native_recv_some(NativeSocketHandle fd, char* data, std::size_t size) {
  while (true) {
#ifdef _WIN32
    const auto chunk = std::min(size, static_cast<std::size_t>(std::numeric_limits<int>::max()));
    const int rc = recv(fd, data, static_cast<int>(chunk), 0);
#else
    const ssize_t rc = recv(fd, data, size, 0);
#endif
    if (rc > 0) return static_cast<std::size_t>(rc);
    if (rc == 0) return std::nullopt;
    const int err = net_last_error();
    if (err == kErrIntr) continue;
    if (err == kErrWouldBlock) {
      const int poll = net_poll(fd, /*want_read=*/true, /*want_write=*/false, 250);
      if (poll < 0) return std::nullopt;
      if (poll == 0) continue;
      continue;
    }
    return std::nullopt;
  }
}

void pipe_bytes_native(NativeSocketHandle from_fd, NativeSocketHandle to_fd, std::atomic<bool>& done) {
  disable_native_sigpipe(to_fd);
  std::array<char, 16 * 1024> buffer{};
  while (!done.load()) {
    auto count = native_recv_some(from_fd, buffer.data(), buffer.size());
    if (!count) break;
    if (!native_send_all(to_fd, buffer.data(), *count)) break;
  }
  done.store(true);
  native_shutdown_both(from_fd);
  native_shutdown_both(to_fd);
}

void relay_stream_sync(TcpSocket first, TcpSocket second) {
  const auto first_fd = first.native_handle();
  const auto second_fd = second.native_handle();
  std::atomic<bool> done{false};
  // Use native full-duplex I/O here; sharing one Asio socket object across two
  // pipe threads for recv/send/close is not a safe ownership model.
  std::thread t1([&] { pipe_bytes_native(first_fd, second_fd, done); });
  std::thread t2([&] { pipe_bytes_native(second_fd, first_fd, done); });
  if (t1.joinable()) t1.join();
  if (t2.joinable()) t2.join();
  first.close();
  second.close();
}

void reject_client(TcpSocket& socket, const std::string& code) {
  send_message(socket, Message{"error", {{"code", code}}});
}

void send_message_best_effort(TcpSocket& socket, const Message& message) {
  try {
    if (socket.valid()) send_message(socket, message);
  } catch (...) {
  }
}

void handle_client(TcpSocket socket, const std::shared_ptr<RelayStateImpl>& state,
                   const std::atomic_bool* cancel) {
  std::string active_room;
  try {
    auto first = recv_message_timeout(socket, kControlReadTimeout, cancel, kMaxRelayControlFrameBytes);
    if (cancel && cancel->load()) return;
    if (first && first->type == "ping") {
      send_message(socket, Message{"pong", {{"version", kVersion}}});
      first = recv_message_timeout(socket, kControlReadTimeout, cancel, kMaxRelayControlFrameBytes);
      if (cancel && cancel->load()) return;
      if (!first) return;
    }
    if (first && first->type == "punch_probe") {
      handle_punch_probe(socket, *first, state);
      return;
    }
    if (!first || first->type != "hello") throw KikoError("expected hello");
    const auto& hello = *first;

    if (!relay_password_ok(state->config_, hello)) {
      reject_client(socket, "bad_password");
      return;
    }

    RelayHello registration;
    try {
      registration = decode_relay_hello(hello);
    } catch (const KikoError&) {
      reject_client(socket, "invalid_hello");
      return;
    }

    auto peer_addr = socket.peer_endpoint();
    RelayWaitingPeer self;
    self.role = registration.role;
    self.socket = std::move(socket);
    if (!registration.punch_public.host.empty() && registration.punch_public.port > 0) {
      self.public_endpoint = registration.punch_public;
    } else {
      self.public_endpoint =
          Endpoint{peer_addr.host, registration.listen.port == 0 ? peer_addr.port : registration.listen.port};
    }
    self.listen_endpoint = registration.listen;
    if (self.listen_endpoint.host.empty()) self.listen_endpoint.host = peer_addr.host;
    self.file_count = registration.file_count;
    self.total_size = registration.total_size;
    self.conn_count = registration.conn_count;
    self.no_direct = registration.no_direct;
    self.local_candidates = std::move(registration.local_candidates);
    self.registered_at = std::chrono::steady_clock::now();

    auto match_key = registration.room + "#" + std::to_string(registration.conn_index);
    active_room = relay_room_base(match_key);

    state->rooms_.purge_stale_waiting();

    auto pairing = state->rooms_.pair_or_wait(match_key, active_room, std::move(self), registration.auxiliary);
    if (pairing.kind == RelayRoomPairing::Kind::Waiting) return;
    if (pairing.kind == RelayRoomPairing::Kind::RoomFull) {
      reject_client(pairing.self->socket, "room_full");
      return;
    }
    if (pairing.kind == RelayRoomPairing::Kind::CapacityFull) {
      reject_client(pairing.self->socket, "server_busy");
      return;
    }

    auto other = std::move(pairing.peer);
    self = std::move(*pairing.self);

    if (registration.auxiliary) {
      relay_stream_sync(std::move(other->socket), std::move(self.socket));
      return;
    }

    struct ActiveRoomCleanup {
      std::shared_ptr<RelayStateImpl> state;
      std::string room;
      ~ActiveRoomCleanup() {
        if (!room.empty()) state->rooms_.release_active_room(room);
      }
    } cleanup{state, pairing.claimed_active_room ? active_room : std::string{}};

    send_peer_messages(*other, self);

    const auto decision = wait_route_decision(other->socket, self.socket, cancel);
    if (decision.kind == RelayRouteDecision::Kind::Direct) {
      send_message(other->socket, Message{"direct_start", {}});
      send_message(self.socket, Message{"direct_start", {}});
      return;
    }

    if (decision.kind == RelayRouteDecision::Kind::Relay) {
      Message start{"relay_start", {{"reason", decision.relay_reason}}};
      send_message(other->socket, start);
      send_message(self.socket, start);
      relay_stream_sync(std::move(other->socket), std::move(self.socket));
      return;
    }

    Message done{"done", {}};
    send_message_best_effort(other->socket, done);
    send_message_best_effort(self.socket, done);
  } catch (const std::exception& error) {
    state->log_client_error(error.what());
  }
}

struct RelayClientWorker {
  std::thread thread;
  std::shared_ptr<std::atomic_bool> done;
  std::shared_ptr<std::atomic_bool> cancel;
  SocketInterruptHandle interrupt;
};

void reap_finished_workers(std::mutex& mutex, std::vector<RelayClientWorker>& workers) {
  std::vector<std::thread> finished;
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto it = workers.begin(); it != workers.end();) {
      if (!it->done->load(std::memory_order_acquire)) {
        ++it;
        continue;
      }
      finished.push_back(std::move(it->thread));
      it = workers.erase(it);
    }
  }
  for (auto& thread : finished) {
    if (thread.joinable()) thread.join();
  }
}

bool start_client_worker(TcpSocket& socket, const std::shared_ptr<RelayStateImpl>& state,
                         std::mutex& mutex, std::vector<RelayClientWorker>& workers) {
  reap_finished_workers(mutex, workers);

  std::lock_guard<std::mutex> lock(mutex);
  const auto limit = state->config_.max_client_workers == 0 ? std::size_t{1}
                                                            : state->config_.max_client_workers;
  if (workers.size() >= limit) return false;

  auto done = std::make_shared<std::atomic_bool>(false);
  auto cancel = std::make_shared<std::atomic_bool>(false);
  auto interrupt = socket.interrupt_handle();
  workers.emplace_back();
  auto& worker = workers.back();
  worker.done = done;
  worker.cancel = cancel;
  worker.interrupt = std::move(interrupt);
  try {
    worker.thread = std::thread([socket = std::move(socket), state, done, cancel]() mutable {
      handle_client(std::move(socket), state, cancel.get());
      done->store(true, std::memory_order_release);
    });
  } catch (...) {
    workers.pop_back();
    throw;
  }
  return true;
}

void accept_loop(TcpListener& listener, const std::shared_ptr<RelayStateImpl>& state, std::atomic<bool>& stop,
                 std::mutex& client_mutex, std::vector<RelayClientWorker>& client_workers) {
  while (!stop.load()) {
    reap_finished_workers(client_mutex, client_workers);
    auto socket = listener.accept(std::chrono::milliseconds(200));
    if (!socket.valid()) continue;
    if (!start_client_worker(socket, state, client_mutex, client_workers)) {
      send_message_best_effort(socket, Message{"error", {{"code", "server_busy"}}});
    }
  }
  reap_finished_workers(client_mutex, client_workers);
}

}  // namespace

struct BackgroundRelay::Impl {
  std::shared_ptr<RelayStateImpl> state;
  std::unique_ptr<TcpListener> listener;
  std::thread accept_thread;
  std::mutex client_mutex;
  std::vector<RelayClientWorker> client_workers;
  std::atomic<bool> stop{false};
  mutable std::mutex failure_mutex;
  std::optional<std::string> failure;

  void set_failure(std::string error) {
    std::lock_guard<std::mutex> lock(failure_mutex);
    failure = std::move(error);
  }

  [[nodiscard]] std::optional<std::string> last_error() const {
    std::lock_guard<std::mutex> lock(failure_mutex);
    return failure;
  }
};

BackgroundRelay::BackgroundRelay() = default;

BackgroundRelay::~BackgroundRelay() { stop(); }

void BackgroundRelay::start(const Endpoint& bind_addr, const RelayServerConfig& config) {
  if (running_.load()) return;
  if (impl_) stop();
  auto listener = TcpListener::bind(bind_addr);
  bound_ = listener.local_endpoint();
  impl_ = std::make_unique<Impl>();
  impl_->state = std::make_shared<RelayStateImpl>(config);
  impl_->listener = std::make_unique<TcpListener>(std::move(listener));
  impl_->stop.store(false);
  running_.store(true);
  auto* relay_impl = impl_.get();
  try {
    impl_->accept_thread = std::thread([this, relay_impl]() {
      try {
        accept_loop(*relay_impl->listener, relay_impl->state, relay_impl->stop,
                    relay_impl->client_mutex, relay_impl->client_workers);
      } catch (const std::exception& error) {
        relay_impl->set_failure(error.what());
        relay_impl->stop.store(true);
        running_.store(false);
      } catch (...) {
        relay_impl->set_failure("unknown accept loop error");
        relay_impl->stop.store(true);
        running_.store(false);
      }
    });
  } catch (...) {
    running_.store(false);
    impl_.reset();
    throw;
  }
}

void BackgroundRelay::stop() {
  running_.store(false);
  if (impl_) {
    impl_->stop.store(true);
    if (impl_->accept_thread.joinable()) impl_->accept_thread.join();
    if (impl_->state) impl_->state->rooms_.close_waiting();
    std::vector<RelayClientWorker> client_workers;
    {
      std::lock_guard<std::mutex> lock(impl_->client_mutex);
      client_workers.swap(impl_->client_workers);
    }
    for (const auto& worker : client_workers) {
      worker.cancel->store(true, std::memory_order_release);
      worker.interrupt.interrupt();
    }
    for (auto& worker : client_workers) {
      if (worker.thread.joinable()) worker.thread.join();
    }
  }
  impl_.reset();
}

Endpoint BackgroundRelay::local_endpoint() const { return bound_; }

std::optional<std::string> BackgroundRelay::last_error() const {
  return impl_ ? impl_->last_error() : std::nullopt;
}

bool relay_password_ok(const RelayServerConfig& config, const Message& hello) {
  if (config.password.empty()) return true;
  return hello.get("relay_pass") == config.password;
}

}  // namespace kiko
