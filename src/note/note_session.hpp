#pragma once

#include "connect/peer_session.hpp"
#include "core/cancellation.hpp"
#include "note/note_workspace.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace kiko {

class NoteSession;

struct NoteSessionInfo {
  std::string code;
  RouteOutcome outcome;
};

enum class NoteSessionEvent {
  Acknowledged,
  RemoteApplied,
  LocalSent,
};

struct NoteSessionCallbacks {
  std::function<void(const NoteSessionInfo&)> connected;
  std::function<void(const NoteSession&, NoteSessionEvent, const NoteFrame&)> workspace_changed;
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
  [[nodiscard]] bool update_active(std::string text) {
    return queue_frame(workspace_.update_active(std::move(text)));
  }
  [[nodiscard]] bool clear_active() { return queue_frame(workspace_.clear_active()); }
  [[nodiscard]] bool create_pad() { return queue_frame(workspace_.create_pad()); }
  [[nodiscard]] bool select_pad(const std::string& pad_id) { return workspace_.select_pad(pad_id); }
  [[nodiscard]] NoteDocument active_document() const { return workspace_.active_document(); }
  [[nodiscard]] std::optional<NoteDocument> document(const std::string& pad_id) const {
    return workspace_.document(pad_id);
  }
  [[nodiscard]] NoteWorkspaceSnapshot snapshot() const { return workspace_.snapshot(); }

  void request_stop();

  [[nodiscard]] bool connected() const;
  [[nodiscard]] std::shared_ptr<TransferCancellation> cancellation() const;

 private:
  [[nodiscard]] bool queue_frame(NoteFrame frame);
  void notify_workspace_changed(NoteSessionEvent event, const NoteFrame& frame) const;
  void sender_loop();
  void close_channel();
  void stop_sender();

  PeerSessionConfig config_;
  ProgressReporter& reporter_;
  NoteSessionCallbacks callbacks_;
  std::shared_ptr<TransferCancellation> cancellation_;
  NoteWorkspace workspace_;

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
