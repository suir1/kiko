#include "notepad.hpp"

#include "note/note_session.hpp"

#include <condition_variable>
#include <exception>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace kiko {
namespace {

void print_note(const NoteDocument& document) {
  std::cout << "\n--- " << (document.title.empty() ? document.pad_id : document.title) << " rev "
            << document.revision << " (" << document.text.size() << " bytes) ---\n";
  if (!document.text.empty()) std::cout << document.text << "\n";
  std::cout << "--- end ---\n> " << std::flush;
}

}  // namespace

int run_note(const PeerSessionConfig& config, ProgressReporter& reporter) {
  std::mutex output_mutex;
  std::mutex connection_mutex;
  std::condition_variable connection_changed;
  bool connected = false;
  bool session_finished = false;
  std::exception_ptr session_error;
  NoteSessionCallbacks callbacks;
  callbacks.connected = [&](const NoteSessionInfo& info) {
    {
      std::lock_guard<std::mutex> lock(connection_mutex);
      connected = true;
    }
    connection_changed.notify_all();
    std::lock_guard<std::mutex> lock(output_mutex);
    std::cout << "notepad connected via " << info.outcome.data_path << "\n";
    std::cout << "Type text and press Enter to append. Commands: /show /clear /quit\n> " << std::flush;
  };
  callbacks.workspace_changed = [&](const NoteSession& session, NoteSessionEvent event,
                                    const NoteFrame& frame) {
    if (event == NoteSessionEvent::Acknowledged) {
      std::lock_guard<std::mutex> lock(output_mutex);
      std::cout << "\npeer synced " << frame.pad_id << " revision " << frame.revision << "\n> " << std::flush;
      return;
    }
    if (event != NoteSessionEvent::RemoteApplied) return;
    const auto document = session.document(frame.pad_id);
    if (!document) return;
    std::lock_guard<std::mutex> lock(output_mutex);
    std::cout << "\nremote updated " << document->pad_id;
    print_note(*document);
  };

  NoteSession session(config, reporter, std::move(callbacks));
  std::thread worker([&] {
    try {
      (void)session.run();
    } catch (...) {
      if (!session.cancellation()->requested()) session_error = std::current_exception();
    }
    {
      std::lock_guard<std::mutex> lock(connection_mutex);
      session_finished = true;
    }
    connection_changed.notify_all();
  });

  {
    std::unique_lock<std::mutex> lock(connection_mutex);
    connection_changed.wait(lock, [&] { return connected || session_finished; });
  }
  if (!connected) {
    if (worker.joinable()) worker.join();
    if (session_error) std::rethrow_exception(session_error);
    reporter.status("notepad closed");
    return 0;
  }

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line == "/quit") break;
    if (line == "/show") {
      std::lock_guard<std::mutex> lock(output_mutex);
      print_note(session.active_document());
      continue;
    }

    bool queued = false;
    if (line == "/clear") {
      queued = session.clear_active();
    } else {
      auto next = session.active_document().text;
      if (!next.empty()) next += "\n";
      next += line;
      if (next.size() > kNoteMaxBytes) {
        std::lock_guard<std::mutex> lock(output_mutex);
        std::cout << "\nnote is over 1 MiB; not synced\n> " << std::flush;
        continue;
      }
      queued = session.update_active(std::move(next));
    }
    if (!queued) break;
    std::lock_guard<std::mutex> lock(output_mutex);
    print_note(session.active_document());
  }

  session.request_stop();
  if (worker.joinable()) worker.join();
  if (session_error) std::rethrow_exception(session_error);
  reporter.status("notepad closed");
  return 0;
}

}  // namespace kiko
