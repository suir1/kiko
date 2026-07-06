#include "notepad.hpp"

#include "note_protocol.hpp"
#include "core/cancellation.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace kiko {
namespace {

constexpr auto kNoteReadPoll = std::chrono::milliseconds(100);
constexpr auto kNoteHelloTimeout = std::chrono::seconds(20);

const std::atomic_bool* note_cancel_flag(const std::shared_ptr<TransferCancellation>& cancellation) {
  return cancellation ? cancellation->flag() : nullptr;
}

std::optional<NoteFrame> recv_note_interruptible(TcpSocket& channel, StreamCipher& cipher, const std::atomic_bool& done,
                                                 const std::atomic_bool* cancel) {
  while (!done.load() && !(cancel && cancel->load())) {
    auto frame = recv_note_frame_timeout(channel, cipher, kNoteReadPoll, cancel);
    if (frame) return frame;
    if (!channel.valid()) return std::nullopt;
  }
  return std::nullopt;
}

void print_note(const NoteDocument& document) {
  std::cout << "\n--- note rev " << document.revision << " (" << document.text.size() << " bytes) ---\n";
  if (!document.text.empty()) std::cout << document.text << "\n";
  std::cout << "--- end ---\n> " << std::flush;
}

}  // namespace

int run_note(const NoteConfig& config, ProgressReporter& reporter) {
  auto session = open_peer_session(config, reporter);
  auto channel = std::move(session.encrypted.channel);
  StreamCipher cipher(session.encrypted.key, config.role == Role::Sender);
  const auto* cancel = note_cancel_flag(config.cancellation);

  send_note_frame(channel, cipher, make_note_hello());
  reporter.status("notepad hello sent");
  auto hello = recv_note_frame_timeout(channel, cipher, kNoteHelloTimeout, cancel);
  if (!hello || hello->type != NoteFrameType::Hello) throw KikoError("note peer did not send hello");
  reporter.status("notepad peer hello received");

  std::mutex mutex;
  std::mutex send_mutex;
  NoteDocument document;
  std::atomic_bool done{false};
  std::exception_ptr receiver_error;

  std::thread receiver([&]() {
    try {
      while (!done.load()) {
        auto frame = recv_note_interruptible(channel, cipher, done, cancel);
        if (!frame) break;
        if (frame->type == NoteFrameType::Bye) break;
        if (frame->type == NoteFrameType::Ack) {
          std::cout << "\npeer synced revision " << frame->revision << "\n> " << std::flush;
          continue;
        }
        {
          std::lock_guard<std::mutex> lock(mutex);
          if (!apply_note_update(document, *frame)) continue;
          std::cout << "\nremote updated the note";
          print_note(document);
        }
        {
          std::lock_guard<std::mutex> lock(send_mutex);
          send_note_frame(channel, cipher, make_note_ack(frame->revision));
        }
      }
    } catch (...) {
      if (!done.load()) receiver_error = std::current_exception();
    }
    done.store(true);
  });

  std::cout << "notepad connected via " << session.outcome.data_path << "\n";
  std::cout << "Type text and press Enter to append. Commands: /show /clear /quit\n> " << std::flush;

  std::string line;
  while (!done.load() && std::getline(std::cin, line)) {
    if (line == "/quit") break;
    NoteFrame frame;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (line == "/show") {
        print_note(document);
        continue;
      }
      if (line == "/clear") {
        frame = make_note_clear(document.revision + 1);
      } else {
        auto next = document.text;
        if (!next.empty()) next += "\n";
        next += line;
        if (next.size() > kNoteMaxBytes) {
          std::cout << "\nnote is over 1 MiB; not synced\n> " << std::flush;
          continue;
        }
        frame = make_note_update(document.revision + 1, std::move(next));
      }
      (void)apply_note_update(document, frame);
      print_note(document);
    }
    {
      std::lock_guard<std::mutex> lock(send_mutex);
      send_note_frame(channel, cipher, frame);
    }
    std::cout << "> " << std::flush;
  }

  done.store(true);
  try {
    NoteFrame bye;
    bye.type = NoteFrameType::Bye;
    bye.timestamp_ms = now_ms();
    std::lock_guard<std::mutex> lock(send_mutex);
    send_note_frame(channel, cipher, bye);
  } catch (...) {
  }
  channel.close();
  if (receiver.joinable()) receiver.join();
  if (receiver_error) std::rethrow_exception(receiver_error);
  reporter.status("notepad closed");
  return 0;
}

}  // namespace kiko
