#include "relay_server.hpp"

#include "adaptive.hpp"
#include "common.hpp"
#include "platform.hpp"
#include "protocol.hpp"
#include "socket.hpp"
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

void pipe_frames_sync(TcpSocket& from, TcpSocket& to, std::atomic<bool>& done) {
  try {
    while (!done.load()) {
      auto frame = recv_frame(from);
      if (!frame) break;
      send_frame(to, *frame);
    }
  } catch (const std::exception&) {
  }
  done.store(true);
  from.close();
  to.close();
}

void relay_stream_sync(TcpSocket first, TcpSocket second) {
  auto done = std::make_shared<std::atomic<bool>>(false);
  std::thread t1([&] { pipe_frames_sync(first, second, *done); });
  std::thread t2([&] { pipe_frames_sync(second, first, *done); });
  if (t1.joinable()) t1.join();
  if (t2.joinable()) t2.join();
}

void reject_client(TcpSocket& socket, const std::string& code) {
  send_message(socket, Message{"error", {{"code", code}}});
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

    auto first_choice = recv_message_timeout(other->socket, kRouteChoiceTimeout);
    auto second_choice = recv_message_timeout(self.socket, kRouteChoiceTimeout);
    if (!first_choice || !second_choice) return;

    if (first_choice->type == "direct_ok" && second_choice->type == "direct_ok") {
      send_message(other->socket, Message{"direct_start", {}});
      send_message(self.socket, Message{"direct_start", {}});
      return;
    }

    const bool first_valid = first_choice->type == "relay_ready" || first_choice->type == "direct_ok";
    const bool second_valid = second_choice->type == "relay_ready" || second_choice->type == "direct_ok";
    if (first_valid && second_valid) {
      const bool mismatch = first_choice->type != second_choice->type;
      Message start{"relay_start", {{"reason", mismatch ? "mismatch" : "relay"}}};
      send_message(other->socket, start);
      send_message(self.socket, start);
      relay_stream_sync(std::move(other->socket), std::move(self.socket));
      return;
    }

    send_message(other->socket, Message{"done", {}});
    send_message(self.socket, Message{"done", {}});
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
