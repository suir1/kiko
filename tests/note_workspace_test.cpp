#include "note/note_workspace.hpp"

#include <cassert>
#include <iostream>

using namespace kiko;

int main() {
  NoteWorkspace workspace;
  auto initial = workspace.snapshot();
  assert(initial.active_pad == "main");
  assert(initial.documents.size() == 1);
  assert(initial.documents[0].title == "Note 1");
  assert(!initial.synced);

  auto main_update = workspace.update_active("hello");
  assert(main_update.pad_id == "main");
  assert(main_update.revision == 1);
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

  std::cout << "note workspace ok\n";
  return 0;
}
