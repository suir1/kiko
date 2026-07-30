#include "note/note_session.hpp"
#include "note/note_workspace.hpp"

#include <cassert>
#include <iostream>

using namespace kiko;

int main() {
  NoteWorkspace workspace("workspace-writer");
  auto initial = workspace.snapshot();
  assert(initial.active_pad == "main");
  assert(initial.documents.size() == 1);
  assert(initial.documents[0].title == "Note 1");
  assert(!initial.synced);

  auto main_update = workspace.update_active("hello");
  assert(main_update.pad_id == "main");
  assert(main_update.revision == 1);
  assert(main_update.writer_id == "workspace-writer");
  assert(workspace.active_document().writer_id == "workspace-writer");
  assert(workspace.active_document().text == "hello");
  assert(!workspace.snapshot().synced);

  workspace.acknowledge(make_note_ack("main", main_update.revision));
  assert(workspace.snapshot().synced);

  auto second_update = workspace.create_pad();
  assert(second_update.pad_id == "pad-2");
  assert(workspace.active_document().title == "Note 2");
  assert(!workspace.snapshot().synced);

  workspace.acknowledge(make_note_ack("main", 99));
  assert(!workspace.snapshot().synced);
  workspace.acknowledge(make_note_ack("pad-2", second_update.revision));
  assert(workspace.snapshot().synced);

  auto remote = make_note_update("pad-3", 4, "remote", "Remote");
  assert(workspace.apply_remote(remote));
  assert(workspace.document("pad-3")->text == "remote");
  assert(workspace.select_pad("pad-3"));
  assert(workspace.active_document().title == "Remote");
  assert(!workspace.select_pad("missing"));

  auto clear = workspace.clear_active();
  assert(clear.pad_id == "pad-3");
  assert(workspace.active_document().text.empty());

  const auto final = workspace.snapshot();
  assert(final.documents.size() == 3);
  assert(final.documents[0].pad_id == "main");

  NoteWorkspace generated_writer;
  assert(!generated_writer.update_active("generated").writer_id.empty());

  NoteWorkspace concurrent_left("writer-a");
  NoteWorkspace concurrent_right("writer-b");
  const auto left_frame = decode_note_frame(encode_note_frame(concurrent_left.update_active("left")));
  const auto right_frame = decode_note_frame(encode_note_frame(concurrent_right.update_active("right")));
  assert(concurrent_left.apply_remote(right_frame));
  assert(!concurrent_right.apply_remote(left_frame));
  assert(concurrent_left.active_document().text == "right");
  assert(concurrent_right.active_document().text == "right");

  ProgressReporter reporter;
  NoteSession session(PeerSessionConfig{}, reporter);
  for (int i = 0; i < 1000; ++i) {
    assert(session.update_active("session-owned-" + std::to_string(i)));
  }
  assert(session.active_document().text == "session-owned-999");
  assert(session.pending_frame_count() == 1);
  assert(session.create_pad());
  assert(session.pending_frame_count() == 2);
  assert(session.active_document().pad_id == "pad-2");
  assert(session.active_document().title == "Note 2");
  assert(session.update_active("second-pad"));
  assert(session.pending_frame_count() == 2);
  assert(session.select_pad("main"));
  assert(session.clear_active());
  assert(session.pending_frame_count() == 2);
  assert(session.pending_frame_bytes() <= kNoteOutboxMaxBytes);
  assert(session.active_document().text.empty());
  assert(session.snapshot().documents.size() == 2);
  session.request_stop();

  NoteSession bounded_session(PeerSessionConfig{}, reporter);
  const std::string full_note(kNoteMaxBytes, 'x');
  bool queue_limit_reached = false;
  for (int i = 0; i < 32; ++i) {
    if (i > 0 && !bounded_session.create_pad()) {
      queue_limit_reached = true;
      break;
    }
    if (!bounded_session.update_active(full_note)) {
      queue_limit_reached = true;
      break;
    }
  }
  assert(queue_limit_reached);
  assert(bounded_session.pending_frame_bytes() <= kNoteOutboxMaxBytes);
  bounded_session.request_stop();

  NoteSession frame_bounded_session(PeerSessionConfig{}, reporter);
  bool frame_limit_reached = false;
  for (std::size_t i = 0; i <= kNoteOutboxMaxFrames; ++i) {
    if (!frame_bounded_session.create_pad()) {
      frame_limit_reached = true;
      break;
    }
  }
  assert(frame_limit_reached);
  assert(frame_bounded_session.pending_frame_count() <= kNoteOutboxMaxFrames);
  frame_bounded_session.request_stop();

  std::cout << "note workspace ok\n";
  return 0;
}
