#pragma once

#include "connect/peer_session.hpp"
#include "core/cancellation.hpp"
#include "note/note_protocol.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace kiko {

struct NoteSessionInfo {
  std::string code;
  RouteOutcome outcome;
};

struct NoteSessionCallbacks {
  std::function<void(const NoteSessionInfo&)> connected;
  std::function<void(const NoteFrame&)> frame_received;
  std::function<void(const NoteFrame&)> frame_sent;
};

enum class NoteSessionEnd {
  PeerClosed,
  Stopped,
};

class NoteSession {
 public:
  NoteSession(PeerSessionConfig config, ProgressReporter& reporter, NoteSessionCallbacks callbacks = {});
  NoteSession(const NoteSession&) = delete;
  NoteSession& operator=(const NoteSession&) = delete;
  ~NoteSession();

  [[nodiscard]] NoteSessionEnd run();
  [[nodiscard]] bool send(NoteFrame frame);
  void request_stop();

  [[nodiscard]] bool connected() const;
  [[nodiscard]] std::shared_ptr<TransferCancellation> cancellation() const;

 private:
  void sender_loop();
  void close_channel();
  void stop_sender();

  PeerSessionConfig config_;
  ProgressReporter& reporter_;
  NoteSessionCallbacks callbacks_;
  std::shared_ptr<TransferCancellation> cancellation_;

  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::unique_ptr<TcpSocket> channel_;
  std::unique_ptr<StreamCipher> cipher_;
  std::deque<NoteFrame> outgoing_;
  std::exception_ptr sender_error_;
  std::thread sender_;
  std::atomic_bool stop_{false};
  std::atomic_bool explicit_stop_{false};
  bool connected_ = false;
};

}  // namespace kiko
