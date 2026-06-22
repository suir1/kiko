#include "relay_server.hpp"

#include "adaptive.hpp"
#include "common.hpp"
#include "platform.hpp"
#include "protocol.hpp"
#include "socket.hpp"
#include <array>
#include <chrono>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace kiko {
namespace {

constexpr auto kControlReadTimeout = std::chrono::seconds(5);
constexpr auto kRouteChoiceTimeout = std::chrono::seconds(15);
constexpr auto kRoutePollSlice = std::chrono::milliseconds(25);
constexpr auto kRouteFrameReadTimeout = std::chrono::milliseconds(250);
constexpr std::uint64_t kMaxRelayConnections = 32;

std::string room_base(const std::string& match_key) {
  const auto hash = match_key.find('#');
  if (hash == std::string::npos) return match_key;
  return match_key.substr(0, hash);
}

bool socket_is_dead(TcpSocket& socket) {
  const int fd = socket.fd();
  if (fd < 0) return true;
  const int poll = net_poll(fd, true, false, 0);
  if (poll < 0) return true;
  if (poll == 0) return false;

  char byte = 0;
#ifdef _WIN32
  const int rc = recv(static_cast<SOCKET>(fd), &byte, 1, MSG_PEEK);
#else
  const ssize_t rc = recv(fd, &byte, 1, MSG_PEEK);
#endif
  if (rc == 0) return true;
  if (rc > 0) return false;
  const int err = net_last_error();
  return err != kErrWouldBlock && err != kErrIntr;
}

enum class RouteChoiceState { Waiting, Standby, DirectOk, RelayReady, Invalid };

struct RouteDecision {
  enum class Kind { Pending, Direct, Relay, Invalid };
  Kind kind = Kind::Pending;
  std::string relay_reason;
};

struct RouteChoiceRead {
  enum class Kind { None, Message, Closed };
  Kind kind = Kind::None;
  Message message;
};

bool has_route_presence(RouteChoiceState state) { return state != RouteChoiceState::Waiting && state != RouteChoiceState::Invalid; }

bool route_choice_final(RouteChoiceState state) {
  return state == RouteChoiceState::DirectOk || state == RouteChoiceState::RelayReady ||
         state == RouteChoiceState::Invalid;
}

RouteChoiceState route_choice_from(const Message& msg) {
  if (msg.type == "relay_standby") return RouteChoiceState::Standby;
  if (msg.type == "direct_ok") return RouteChoiceState::DirectOk;
  if (msg.type == "relay_ready") return RouteChoiceState::RelayReady;
  return RouteChoiceState::Invalid;
}

void merge_route_choice(RouteChoiceState& state, RouteChoiceState next) {
  if (state == RouteChoiceState::Invalid || route_choice_final(state)) return;
  if (next == RouteChoiceState::Invalid || route_choice_final(next) || state == RouteChoiceState::Waiting) {
    state = next;
  }
}

RouteChoiceRead recv_route_choice_if_ready(TcpSocket& socket) {
  const int fd = socket.fd();
  if (fd < 0) return {RouteChoiceRead::Kind::Closed, {}};
  const int poll = net_poll(fd, /*want_read=*/true, /*want_write=*/false, 0);
  if (poll == 0) return {};
  if (poll < 0) return {RouteChoiceRead::Kind::Closed, {}};
  auto message = recv_message_timeout(socket, kRouteFrameReadTimeout);
  if (!message) return {RouteChoiceRead::Kind::Closed, {}};
  return {RouteChoiceRead::Kind::Message, std::move(*message)};
}

RouteDecision route_decision_for(RouteChoiceState first, RouteChoiceState second, bool deadline_expired) {
  if (first == RouteChoiceState::Invalid || second == RouteChoiceState::Invalid) {
    return {RouteDecision::Kind::Invalid, {}};
  }
  if (first == RouteChoiceState::DirectOk && second == RouteChoiceState::DirectOk) {
    return {RouteDecision::Kind::Direct, {}};
  }

  const bool first_relay = first == RouteChoiceState::RelayReady;
  const bool second_relay = second == RouteChoiceState::RelayReady;
  if ((first_relay && has_route_presence(second)) || (second_relay && has_route_presence(first))) {
    if (first_relay && second_relay) return {RouteDecision::Kind::Relay, "relay"};
    if (first == RouteChoiceState::DirectOk || second == RouteChoiceState::DirectOk) {
      return {RouteDecision::Kind::Relay, "mismatch"};
    }
    return {RouteDecision::Kind::Relay, "standby"};
  }

  if (deadline_expired && has_route_presence(first) && has_route_presence(second)) {
    return {RouteDecision::Kind::Relay, "timeout"};
  }
  return {};
}

RouteDecision wait_route_decision(TcpSocket& first, TcpSocket& second) {
  RouteChoiceState first_choice = RouteChoiceState::Waiting;
  RouteChoiceState second_choice = RouteChoiceState::Waiting;
  const auto deadline = std::chrono::steady_clock::now() + kRouteChoiceTimeout;

  while (std::chrono::steady_clock::now() < deadline) {
    const auto first_read = recv_route_choice_if_ready(first);
    if (first_read.kind == RouteChoiceRead::Kind::Message) {
      merge_route_choice(first_choice, route_choice_from(first_read.message));
    } else if (first_read.kind == RouteChoiceRead::Kind::Closed) {
      merge_route_choice(first_choice, RouteChoiceState::Invalid);
    }
    const auto second_read = recv_route_choice_if_ready(second);
    if (second_read.kind == RouteChoiceRead::Kind::Message) {
      merge_route_choice(second_choice, route_choice_from(second_read.message));
    } else if (second_read.kind == RouteChoiceRead::Kind::Closed) {
      merge_route_choice(second_choice, RouteChoiceState::Invalid);
    }

    auto decision = route_decision_for(first_choice, second_choice, /*deadline_expired=*/false);
    if (decision.kind != RouteDecision::Kind::Pending) return decision;
    std::this_thread::sleep_for(kRoutePollSlice);
  }

  return route_decision_for(first_choice, second_choice, /*deadline_expired=*/true);
}

std::uint16_t checked_control_port(const Message& message, const std::string& field, std::uint16_t fallback,
                                   bool allow_zero) {
  const auto value = message.get(field);
  if (value.empty()) return fallback;
  const auto port = message.get_u64(field, fallback);
  if (port > 65535 || (!allow_zero && port == 0)) throw KikoError("invalid port field: " + field);
  return static_cast<std::uint16_t>(port);
}

struct WaitingPeer {
  Role role = Role::Sender;
  TcpSocket socket;
  Endpoint public_endpoint;
  Endpoint listen_endpoint;
  std::uint64_t file_count = 0;
  std::uint64_t total_size = 0;
  std::uint64_t conn_count = 1;
  bool no_direct = false;
  std::string local_candidates;
  std::chrono::steady_clock::time_point registered_at = std::chrono::steady_clock::now();
};

class RelayStateImpl {
 public:
  explicit RelayStateImpl(RelayServerConfig config) : config_(std::move(config)) { start_cleanup(); }

  ~RelayStateImpl() { stop_cleanup(); }

  [[nodiscard]] bool check_password(const Message& hello) const {
    return relay_password_ok(config_, hello);
  }

  [[nodiscard]] bool is_room_active(const std::string& room) {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_rooms_.count(room) > 0;
  }

  void mark_room_active(const std::string& room) {
    std::lock_guard<std::mutex> lock(mutex_);
    active_rooms_.insert(room);
  }

  void unmark_room_active(const std::string& room) {
    std::lock_guard<std::mutex> lock(mutex_);
    active_rooms_.erase(room);
  }

  void close_waiting() {
    std::vector<TcpSocket> to_close;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto& [_, peer] : waiting_) {
        if (peer.socket.valid()) to_close.push_back(std::move(peer.socket));
      }
      waiting_.clear();
    }
    for (auto& socket : to_close) socket.close();
  }

  RelayServerConfig config_;
  std::mutex mutex_;
  std::map<std::string, WaitingPeer> waiting_;
  std::set<std::string> active_rooms_;
  std::atomic<bool> cleanup_stop_{false};
  std::thread cleanup_thread_;

  void purge_stale_waiting() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<TcpSocket> to_close;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto it = waiting_.begin(); it != waiting_.end();) {
        const bool expired = config_.room_ttl.count() > 0 && now - it->second.registered_at > config_.room_ttl;
        const int fd = it->second.socket.fd();
        const bool dead = socket_is_dead(it->second.socket);
        if (expired || dead) {
          if (fd >= 0) to_close.push_back(std::move(it->second.socket));
          it = waiting_.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (auto& sock : to_close) {
      try {
        send_message(sock, Message{"error", {{"code", "room_expired"}}});
      } catch (...) {
      }
      sock.close();
    }
  }

 private:
  void start_cleanup() {
    cleanup_thread_ = std::thread([this]() {
      while (!cleanup_stop_.load()) {
        const auto interval = config_.cleanup_interval.count() > 0 ? config_.cleanup_interval
                                                                   : std::chrono::seconds(60);
        for (int i = 0; i < interval.count() && !cleanup_stop_.load(); ++i) {
          std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (cleanup_stop_.load()) break;
        purge_stale_waiting();
      }
    });
  }

  void stop_cleanup() {
    cleanup_stop_.store(true);
    if (cleanup_thread_.joinable()) cleanup_thread_.join();
  }

  void purge_expired_waiting() { purge_stale_waiting(); }
};

void send_peer_messages(WaitingPeer& a, WaitingPeer& b) {
  const auto punch_token = std::to_string(now_ms() + 250);
  send_message(a.socket,
               Message{"peer",
                       {{"peer_public_host", b.public_endpoint.host},
                        {"peer_public_port", std::to_string(b.public_endpoint.port)},
                        {"peer_listen_host", b.listen_endpoint.host},
                        {"peer_listen_port", std::to_string(b.listen_endpoint.port)},
                        {"peer_local_candidates", b.local_candidates},
                        {"peer_no_direct", b.no_direct ? "1" : "0"},
                        {"your_public_host", a.public_endpoint.host},
                        {"your_public_port", std::to_string(a.public_endpoint.port)},
                        {"punch_token", punch_token},
                        {"route_commit", "v2"},
                        {"file_count", std::to_string(b.file_count)},
                        {"total_size", std::to_string(b.total_size)},
                        {"conn_count", std::to_string(b.conn_count)}}});
  send_message(b.socket,
               Message{"peer",
                       {{"peer_public_host", a.public_endpoint.host},
                        {"peer_public_port", std::to_string(a.public_endpoint.port)},
                        {"peer_listen_host", a.listen_endpoint.host},
                        {"peer_listen_port", std::to_string(a.listen_endpoint.port)},
                        {"peer_local_candidates", a.local_candidates},
                        {"peer_no_direct", a.no_direct ? "1" : "0"},
                        {"your_public_host", b.public_endpoint.host},
                        {"your_public_port", std::to_string(b.public_endpoint.port)},
                        {"punch_token", punch_token},
                        {"route_commit", "v2"},
                        {"file_count", std::to_string(a.file_count)},
                        {"total_size", std::to_string(a.total_size)},
                        {"conn_count", std::to_string(a.conn_count)}}});
}

void handle_punch_probe(TcpSocket& socket, const Message& probe, const std::shared_ptr<RelayStateImpl>& state) {
  if (!state->check_password(probe)) {
    send_message(socket, Message{"error", {{"code", "bad_password"}}});
    return;
  }
  const auto peer_addr = socket.peer_endpoint();
  send_message(socket, Message{"punch_observed",
                               {{"public_host", peer_addr.host}, {"public_port", std::to_string(peer_addr.port)}}});
}

void native_shutdown_both(int fd) {
  if (fd < 0) return;
#ifdef _WIN32
  shutdown(static_cast<SOCKET>(fd), SD_BOTH);
#else
  ::shutdown(fd, SHUT_RDWR);
#endif
}

void disable_native_sigpipe(int fd) {
#if !defined(_WIN32) && defined(SO_NOSIGPIPE)
  int enabled = 1;
  (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#else
  (void)fd;
#endif
}

bool native_send_all(int fd, const char* data, std::size_t size) {
  std::size_t sent = 0;
  while (sent < size) {
#ifdef _WIN32
    const int rc = send(static_cast<SOCKET>(fd), data + sent, static_cast<int>(size - sent), 0);
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

std::optional<std::size_t> native_recv_some(int fd, char* data, std::size_t size) {
  while (true) {
#ifdef _WIN32
    const int rc = recv(static_cast<SOCKET>(fd), data, static_cast<int>(size), 0);
#else
    const ssize_t rc = recv(fd, data, size, 0);
#endif
    if (rc > 0) return static_cast<std::size_t>(rc);
    if (rc == 0) return std::nullopt;
    const int err = net_last_error();
    if (err == kErrIntr) continue;
    if (err == kErrWouldBlock) {
      if (net_poll(fd, /*want_read=*/true, /*want_write=*/false, 250) <= 0) return std::nullopt;
      continue;
    }
    return std::nullopt;
  }
}

void pipe_bytes_native(int from_fd, int to_fd, std::atomic<bool>& done) {
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
  const int first_fd = first.fd();
  const int second_fd = second.fd();
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

void handle_client(TcpSocket socket, const std::shared_ptr<RelayStateImpl>& state) {
  std::string active_room;
  try {
    auto first = recv_message_timeout(socket, kControlReadTimeout);
    if (first && first->type == "ping") {
      send_message(socket, Message{"pong", {}});
      first = recv_message_timeout(socket, kControlReadTimeout);
      if (!first) return;
    }
    if (first && first->type == "punch_probe") {
      handle_punch_probe(socket, *first, state);
      return;
    }
    if (!first || first->type != "hello") throw KikoError("expected hello");
    const auto& hello = *first;

    if (!state->check_password(hello)) {
      reject_client(socket, "bad_password");
      return;
    }

    auto room = hello.get("room");
    if (room.empty()) {
      reject_client(socket, "invalid_hello");
      return;
    }

    Role role = Role::Sender;
    std::uint64_t conn_index = 0;
    std::uint16_t listen_port = 0;
    std::uint64_t file_count = 0;
    std::uint64_t total_size = 0;
    std::uint64_t conn_count = 1;
    std::uint16_t punch_port = 0;
    try {
      role = parse_role(hello.get("role"));
      conn_index = hello.get_u64("conn_index", 0);
      listen_port = checked_control_port(hello, "listen_port", 0, true);
      punch_port = checked_control_port(hello, "punch_public_port", 0, true);
      file_count = hello.get_u64("file_count", 0);
      total_size = hello.get_u64("total_size", 0);
      conn_count = hello.get_u64("conn_count", 1);
      if (conn_count == 0 || conn_count > kMaxRelayConnections) throw KikoError("invalid conn_count");
    } catch (const KikoError&) {
      reject_client(socket, "invalid_hello");
      return;
    }

    bool aux = hello.get("aux") == "1" || conn_index > 0;
    auto peer_addr = socket.peer_endpoint();
    WaitingPeer self;
    self.role = role;
    self.socket = std::move(socket);
    const auto punch_host = hello.get("punch_public_host");
    if (!punch_host.empty() && punch_port > 0) {
      self.public_endpoint = Endpoint{punch_host, punch_port};
    } else {
      self.public_endpoint = Endpoint{peer_addr.host, listen_port == 0 ? peer_addr.port : listen_port};
    }
    self.listen_endpoint = Endpoint{hello.get("listen_host", peer_addr.host), listen_port};
    self.file_count = file_count;
    self.total_size = total_size;
    self.conn_count = conn_count;
    self.no_direct = hello.get("no_direct") == "1";
    self.local_candidates = hello.get("local_candidates");
    self.registered_at = std::chrono::steady_clock::now();

    auto match_key = room + "#" + std::to_string(conn_index);
    active_room = room_base(match_key);

    if (!aux && state->is_room_active(active_room)) {
      reject_client(self.socket, "room_full");
      return;
    }

    state->purge_stale_waiting();

    std::optional<WaitingPeer> other;
    {
      std::lock_guard<std::mutex> lock(state->mutex_);
      auto it = state->waiting_.find(match_key);
      if (it == state->waiting_.end()) {
        state->waiting_.emplace(match_key, std::move(self));
        return;
      }
      if (it->second.role == self.role) {
        reject_client(self.socket, "room_full");
        return;
      }
      other = std::move(it->second);
      state->waiting_.erase(it);
    }

    if (aux) {
      relay_stream_sync(std::move(other->socket), std::move(self.socket));
      return;
    }

    state->mark_room_active(active_room);
    struct ActiveRoomCleanup {
      std::shared_ptr<RelayStateImpl> state;
      std::string room;
      ~ActiveRoomCleanup() {
        if (!room.empty()) state->unmark_room_active(room);
      }
    } cleanup{state, active_room};

    send_peer_messages(*other, self);

    const auto decision = wait_route_decision(other->socket, self.socket);
    if (decision.kind == RouteDecision::Kind::Direct) {
      send_message(other->socket, Message{"direct_start", {}});
      send_message(self.socket, Message{"direct_start", {}});
      return;
    }

    if (decision.kind == RouteDecision::Kind::Relay) {
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
    std::cerr << "relay client error: " << error.what() << "\n";
  }
}

void accept_loop(TcpListener& listener, const std::shared_ptr<RelayStateImpl>& state, std::atomic<bool>& stop,
                 std::mutex& client_mutex, std::vector<std::thread>& client_threads) {
  while (!stop.load()) {
    auto socket = listener.accept(std::chrono::milliseconds(200));
    if (!socket.valid()) continue;
    std::thread client([socket = std::move(socket), state]() mutable { handle_client(std::move(socket), state); });
    std::lock_guard<std::mutex> lock(client_mutex);
    client_threads.push_back(std::move(client));
  }
}

}  // namespace

struct BackgroundRelay::Impl {
  RelayServerConfig config;
  std::shared_ptr<RelayStateImpl> state;
  std::unique_ptr<TcpListener> listener;
  std::thread accept_thread;
  std::mutex client_mutex;
  std::vector<std::thread> client_threads;
  std::atomic<bool> stop{false};
};

BackgroundRelay::BackgroundRelay() = default;

BackgroundRelay::~BackgroundRelay() { stop(); }

void BackgroundRelay::start(const Endpoint& bind_addr, const RelayServerConfig& config) {
  if (running_.load()) return;
  auto listener = TcpListener::bind(bind_addr);
  bound_ = listener.local_endpoint();
  impl_ = std::make_unique<Impl>();
  impl_->config = config;
  impl_->state = std::make_shared<RelayStateImpl>(config);
  impl_->listener = std::make_unique<TcpListener>(std::move(listener));
  impl_->stop.store(false);
  impl_->accept_thread = std::thread([this]() {
    accept_loop(*impl_->listener, impl_->state, impl_->stop, impl_->client_mutex, impl_->client_threads);
  });
  running_.store(true);
}

void BackgroundRelay::stop() {
  if (!running_.exchange(false)) return;
  if (impl_) {
    impl_->stop.store(true);
    if (impl_->accept_thread.joinable()) impl_->accept_thread.join();
    if (impl_->state) impl_->state->close_waiting();
    std::vector<std::thread> client_threads;
    {
      std::lock_guard<std::mutex> lock(impl_->client_mutex);
      client_threads.swap(impl_->client_threads);
    }
    for (auto& thread : client_threads) {
      if (thread.joinable()) thread.join();
    }
  }
  impl_.reset();
}

Endpoint BackgroundRelay::local_endpoint() const { return bound_; }

bool relay_password_ok(const RelayServerConfig& config, const Message& hello) {
  if (config.password.empty()) return true;
  return hello.get("relay_pass") == config.password;
}

void ensure_io_thread() {}
void release_io_thread() {}

}  // namespace kiko
