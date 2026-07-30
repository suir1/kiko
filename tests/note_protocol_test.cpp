#include "note/note_protocol.hpp"

#include <cassert>
#include <iostream>

using namespace kiko;

int main() {
  NoteDocument document;
  auto first = make_note_update("main", 1, "hello");
  assert(apply_note_update(document, first));
  assert(document.text == "hello");
  assert(document.revision == 1);

  auto stale = make_note_update("main", 1, "stale");
  stale.timestamp_ms = first.timestamp_ms - 1;
  assert(!apply_note_update(document, stale));
  assert(document.text == "hello");

  auto newer = make_note_update("main", 2, "hello\nworld");
  assert(apply_note_update(document, newer));
  assert(document.text == "hello\nworld");

  auto clear = make_note_clear("main", 3);
  assert(apply_note_update(document, clear));
  assert(document.text.empty());
  assert(document.revision == 3);

  auto ack = make_note_ack("main", 3);
  assert(ack.type == NoteFrameType::Ack);
  assert(ack.revision == 3);
  assert(!apply_note_update(document, ack));

  NoteDocument second_pad;
  auto second = make_note_update("pad-2", 1, "second note", "Note 2");
  assert(apply_note_update(second_pad, second));
  assert(second_pad.pad_id == "pad-2");
  assert(second_pad.title == "Note 2");
  assert(second_pad.text == "second note");
  auto second_ack = make_note_ack("pad-2", second_pad.revision);
  assert(second_ack.pad_id == "pad-2");

  NoteDocument concurrent_left;
  NoteDocument concurrent_right;
  auto left_update = make_note_update("main", 1, "left", {}, "writer-a");
  auto right_update = make_note_update("main", 1, "right", {}, "writer-b");
  left_update.timestamp_ms = 1234;
  right_update.timestamp_ms = 1234;
  const auto round_trip = decode_note_frame(encode_note_frame(right_update));
  assert(round_trip.writer_id == "writer-b");
  assert(round_trip.timestamp_ms == 1234);
  assert(round_trip.text == "right");
  assert(apply_note_update(concurrent_left, left_update));
  assert(apply_note_update(concurrent_right, right_update));
  (void)apply_note_update(concurrent_left, right_update);
  (void)apply_note_update(concurrent_right, left_update);
  if (concurrent_left.text != "right" || concurrent_right.text != "right" ||
      concurrent_left.writer_id != "writer-b" || concurrent_right.writer_id != "writer-b") {
    std::cerr << "FAIL: equal-time concurrent note updates did not converge\n";
    return 1;
  }

  NoteDocument legacy_left;
  NoteDocument legacy_right;
  auto legacy_alpha = make_note_update("main", 1, "alpha");
  auto legacy_omega = make_note_update("main", 1, "omega");
  legacy_alpha.timestamp_ms = 5678;
  legacy_omega.timestamp_ms = 5678;
  assert(apply_note_update(legacy_left, legacy_alpha));
  assert(apply_note_update(legacy_right, legacy_omega));
  (void)apply_note_update(legacy_left, legacy_omega);
  (void)apply_note_update(legacy_right, legacy_alpha);
  if (legacy_left.text != "omega" || legacy_right.text != "omega") {
    std::cerr << "FAIL: legacy concurrent note updates did not converge\n";
    return 1;
  }

  bool oversized = false;
  try {
    (void)make_note_update("main", 4, std::string(kNoteMaxBytes + 1, 'x'));
  } catch (...) {
    oversized = true;
  }
  assert(oversized);

  std::cout << "note protocol ok\n";
  return 0;
}
