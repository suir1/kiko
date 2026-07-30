#include "note/note_session.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>
#include <utility>

namespace kiko {
namespace {

constexpr auto kNoteReadPoll = std::chrono::milliseconds(100);
constexpr auto kNoteHelloTimeout = std::chrono::seconds(20);

bool is_note_edit(const NoteFrame& frame) {
  return frame.type == NoteFrameType::Update || frame.type == NoteFrameType::Clear;
}

bool frames_coalesce(const NoteFrame& queued, const NoteFrame& incoming) {
  if (queued.pad_id != incoming.pad_id) return false;
  if (is_note_edit(queued) && is_note_edit(incoming)) return true;
  return queued.type == NoteFrameType::Ack && incoming.type == NoteFrameType::Ack;
}

std::size_t queued_frame_bytes(const NoteFrame& frame) {
  std::size_t total = sizeof(NoteFrame);
  for (const auto capacity : {frame.writer_id.capacity(), frame.pad_id.capacity(), frame.title.capacity(),
                              frame.text.capacity()}) {
    if (capacity > std::numeric_limits<std::size_t>::max() - total) {
      return std::numeric_limits<std::size_t>::max();
    }
    total += capacity;
  }
  return total;
}

}  // namespace

NoteSession::NoteSession(PeerSessionConfig config, ProgressReporter& reporter, NoteSessionCallbacks callbacks)
    : config_(std::move(config)),
      reporter_(reporter),
      callbacks_(std::move(callbacks)),
      cancellation_(config_.cancellation ? config_.cancellation : std::make_shared<TransferCancellation>()) {
  config_.app = "note";
  config_.cancellation = cancellation_;
}

NoteSession::~NoteSession() {
  request_stop();
  stop_sender();
  close_channel();
}

NoteSessionEnd NoteSession::run() {
  sender_ = std::thread([this] { sender_loop(); });
  auto finish_run = [this] {
    stop_.store(true);
    interrupt_channel();
    changed_.notify_all();
    stop_sender();
    close_channel();
    std::exception_ptr error;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      error = sender_error_;
    }
    if (error) std::rethrow_exception(error);
  };
  try {
    auto peer = open_peer_session(config_, reporter_);
    auto channel = std::make_unique<TcpSocket>(std::move(peer.channel));
    auto cipher = std::make_unique<StreamCipher>(peer.key, config_.role == Role::Sender);

    send_note_frame(*channel, *cipher, make_note_hello());
    reporter_.status("notepad hello sent");
    auto hello = recv_note_frame_timeout(*channel, *cipher, kNoteHelloTimeout, cancellation_->flag());
    if (!hello) throw KikoError("note peer did not send hello");
    validate_note_hello(*hello);
    reporter_.status("notepad peer hello received");

    {
      std::lock_guard<std::mutex> lock(mutex_);
      channel_ = std::move(channel);
      cipher_ = std::move(cipher);
      connected_ = true;
    }
    changed_.notify_all();
    if (callbacks_.connected) callbacks_.connected(NoteSessionInfo{peer.code, peer.outcome});

    while (!stop_.load() && !cancellation_->requested()) {
      TcpSocket* active_channel = nullptr;
      StreamCipher* active_cipher = nullptr;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        active_channel = channel_.get();
        active_cipher = cipher_.get();
      }
      if (!active_channel || !active_cipher || !active_channel->valid()) break;

      auto frame = recv_note_frame_timeout(*active_channel, *active_cipher, kNoteReadPoll, cancellation_->flag());
      if (!frame) {
        if (!active_channel->valid()) break;
        continue;
      }
      if (frame->type == NoteFrameType::Bye) {
        break;
      }
      if (frame->type == NoteFrameType::Ack) {
        workspace_.acknowledge(*frame);
        notify_workspace_changed(NoteSessionEvent::Acknowledged, *frame);
      } else if (workspace_.apply_remote(*frame)) {
        notify_workspace_changed(NoteSessionEvent::RemoteApplied, *frame);
      }
      if (frame->type == NoteFrameType::Update || frame->type == NoteFrameType::Clear) {
        (void)queue_frame(make_note_ack(frame->pad_id, frame->revision));
      }
    }

    finish_run();
    if (explicit_stop_.load() || cancellation_->requested()) return NoteSessionEnd::Stopped;
    return NoteSessionEnd::PeerClosed;
  } catch (...) {
    finish_run();
    throw;
  }
}

bool NoteSession::queue_frame(NoteFrame frame) {
  bool overflow = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_.load()) return false;

    const auto frame_bytes = queued_frame_bytes(frame);
    const auto queued = std::find_if(outgoing_.begin(), outgoing_.end(),
                                     [&](const NoteFrame& candidate) { return frames_coalesce(candidate, frame); });
    if (queued != outgoing_.end()) {
      if (frame.revision < queued->revision) return true;
      const auto queued_bytes = queued_frame_bytes(*queued);
      const auto remaining_bytes = outgoing_bytes_ >= queued_bytes ? outgoing_bytes_ - queued_bytes : 0;
      if (frame_bytes > kNoteOutboxMaxBytes || remaining_bytes > kNoteOutboxMaxBytes - frame_bytes) {
        overflow = true;
      } else {
        *queued = std::move(frame);
        outgoing_bytes_ = remaining_bytes + frame_bytes;
      }
    } else if (outgoing_.size() >= kNoteOutboxMaxFrames || frame_bytes > kNoteOutboxMaxBytes ||
               outgoing_bytes_ > kNoteOutboxMaxBytes - frame_bytes) {
      overflow = true;
    } else {
      outgoing_.push_back(std::move(frame));
      outgoing_bytes_ += frame_bytes;
    }

    if (overflow) {
      if (!sender_error_) {
        sender_error_ = std::make_exception_ptr(KikoError("notepad send queue limit exceeded"));
      }
      stop_.store(true);
    }
  }

  if (overflow) {
    if (cancellation_) cancellation_->request();
    interrupt_channel();
    changed_.notify_all();
    return false;
  }
  changed_.notify_one();
  return true;
}

std::size_t NoteSession::pending_frame_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return outgoing_.size();
}

std::size_t NoteSession::pending_frame_bytes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return outgoing_bytes_;
}

void NoteSession::request_stop() {
  explicit_stop_.store(true);
  stop_.store(true);
  if (cancellation_) cancellation_->request();
  interrupt_channel();
  changed_.notify_all();
}

bool NoteSession::connected() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return connected_ && channel_ && channel_->valid() && !stop_.load();
}

std::shared_ptr<TransferCancellation> NoteSession::cancellation() const {
  return cancellation_;
}

void NoteSession::notify_workspace_changed(NoteSessionEvent event, const NoteFrame& frame) const {
  if (callbacks_.workspace_changed) callbacks_.workspace_changed(*this, event, frame);
}

void NoteSession::sender_loop() {
  try {
    while (true) {
      NoteFrame frame;
      TcpSocket* channel = nullptr;
      StreamCipher* cipher = nullptr;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [&] {
          return stop_.load() || (channel_ && cipher_ && !outgoing_.empty());
        });
        if (stop_.load()) break;
        if (!channel_ || !cipher_ || outgoing_.empty()) continue;
        const auto frame_bytes = queued_frame_bytes(outgoing_.front());
        frame = std::move(outgoing_.front());
        outgoing_.pop_front();
        outgoing_bytes_ = outgoing_bytes_ >= frame_bytes ? outgoing_bytes_ - frame_bytes : 0;
        channel = channel_.get();
        cipher = cipher_.get();
      }
      send_note_frame(*channel, *cipher, frame);
      if (frame.type == NoteFrameType::Update || frame.type == NoteFrameType::Clear) {
        notify_workspace_changed(NoteSessionEvent::LocalSent, frame);
      }
    }
  } catch (...) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      sender_error_ = std::current_exception();
    }
    stop_.store(true);
    if (cancellation_) cancellation_->request();
    interrupt_channel();
    changed_.notify_all();
  }
}

void NoteSession::interrupt_channel() {
  std::lock_guard<std::mutex> lock(mutex_);
  connected_ = false;
  if (channel_) channel_->interrupt();
}

void NoteSession::close_channel() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (channel_) channel_->close();
}

void NoteSession::stop_sender() {
  changed_.notify_all();
  if (sender_.joinable() && sender_.get_id() != std::this_thread::get_id()) sender_.join();
}

}  // namespace kiko
