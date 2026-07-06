#include "note/note_protocol.hpp"

#include <cassert>
#include <iostream>

using namespace kiko;

int main() {
  NoteDocument document;
  auto first = make_note_update(1, "hello");
  assert(apply_note_update(document, first));
  assert(document.text == "hello");
  assert(document.revision == 1);

  auto stale = make_note_update(1, "stale");
  stale.timestamp_ms = first.timestamp_ms;
  assert(!apply_note_update(document, stale));
  assert(document.text == "hello");

  auto newer = make_note_update(2, "hello\nworld");
  assert(apply_note_update(document, newer));
  assert(document.text == "hello\nworld");

  auto clear = make_note_clear(3);
  assert(apply_note_update(document, clear));
  assert(document.text.empty());
  assert(document.revision == 3);

  auto ack = make_note_ack(3);
  assert(ack.type == NoteFrameType::Ack);
  assert(ack.revision == 3);
  assert(!apply_note_update(document, ack));

  bool oversized = false;
  try {
    (void)make_note_update(4, std::string(kNoteMaxBytes + 1, 'x'));
  } catch (...) {
    oversized = true;
  }
  assert(oversized);

  std::cout << "note protocol ok\n";
  return 0;
}
