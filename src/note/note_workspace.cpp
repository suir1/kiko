#include "note/note_workspace.hpp"

#include <algorithm>
#include <utility>

namespace kiko {
namespace {

std::string normalized_pad_id(const std::string& pad_id) {
  return pad_id.empty() ? std::string("main") : pad_id;
}

std::string default_pad_title(const std::string& pad_id) {
  return pad_id == "main" ? std::string("Note 1") : pad_id;
}

bool pad_before(const NoteDocument& a, const NoteDocument& b) {
  if (a.pad_id == "main") return b.pad_id != "main";
  if (b.pad_id == "main") return false;
  return a.pad_id < b.pad_id;
}

}  // namespace

NoteWorkspace::NoteWorkspace() {
  (void)ensure_pad_locked("main", "Note 1");
}

NoteFrame NoteWorkspace::update_active(std::string text) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto& document = ensure_pad_locked(active_pad_);
  return apply_local_locked(
      document, make_note_update(document.pad_id, document.revision + 1, std::move(text), document.title));
}

NoteFrame NoteWorkspace::clear_active() {
  std::lock_guard<std::mutex> lock(mutex_);
  auto& document = ensure_pad_locked(active_pad_);
  return apply_local_locked(document, make_note_clear(document.pad_id, document.revision + 1, document.title));
}

NoteFrame NoteWorkspace::create_pad() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string pad_id;
  int number = 0;
  do {
    number = next_pad_number_++;
    pad_id = "pad-" + std::to_string(number);
  } while (documents_.contains(pad_id));

  const auto title = "Note " + std::to_string(number);
  auto& document = ensure_pad_locked(pad_id, title);
  active_pad_ = pad_id;
  return apply_local_locked(
      document, make_note_update(document.pad_id, document.revision + 1, document.text, document.title));
}

bool NoteWorkspace::select_pad(const std::string& pad_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto id = normalized_pad_id(pad_id);
  if (!documents_.contains(id)) return false;
  active_pad_ = id;
  return true;
}

bool NoteWorkspace::apply_remote(const NoteFrame& frame) {
  if (frame.type != NoteFrameType::Update && frame.type != NoteFrameType::Clear) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  auto& document = ensure_pad_locked(frame.pad_id, frame.title);
  return apply_note_update(document, frame);
}

void NoteWorkspace::acknowledge(const NoteFrame& frame) {
  if (frame.type != NoteFrameType::Ack) return;
  std::lock_guard<std::mutex> lock(mutex_);
  const auto id = normalized_pad_id(frame.pad_id);
  auto& revision = acked_revisions_[id];
  revision = std::max(revision, frame.revision);
}

NoteDocument NoteWorkspace::active_document() const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = documents_.find(active_pad_);
  return it == documents_.end() ? NoteDocument{} : it->second;
}

std::optional<NoteDocument> NoteWorkspace::document(const std::string& pad_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = documents_.find(normalized_pad_id(pad_id));
  if (it == documents_.end()) return std::nullopt;
  return it->second;
}

NoteWorkspaceSnapshot NoteWorkspace::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  NoteWorkspaceSnapshot out;
  out.active_pad = active_pad_;
  out.documents.reserve(documents_.size());
  for (const auto& [_, document] : documents_) out.documents.push_back(document);
  std::sort(out.documents.begin(), out.documents.end(), pad_before);

  for (const auto& [_, revision] : local_revisions_) {
    out.latest_local_revision = std::max(out.latest_local_revision, revision);
  }
  for (const auto& [_, revision] : acked_revisions_) {
    out.last_acked_revision = std::max(out.last_acked_revision, revision);
  }
  out.has_local_updates = !local_revisions_.empty();
  out.synced = out.has_local_updates && synced_locked();
  return out;
}

NoteFrame NoteWorkspace::apply_local_locked(NoteDocument& document, NoteFrame frame) {
  (void)apply_note_update(document, frame);
  local_revisions_[document.pad_id] = frame.revision;
  return frame;
}

NoteDocument& NoteWorkspace::ensure_pad_locked(const std::string& pad_id, const std::string& title) {
  const auto id = normalized_pad_id(pad_id);
  auto [it, inserted] = documents_.try_emplace(id);
  if (inserted) {
    it->second.pad_id = id;
    it->second.title = title.empty() ? default_pad_title(id) : title;
  } else if (!title.empty()) {
    it->second.title = title;
  }
  return it->second;
}

bool NoteWorkspace::synced_locked() const {
  for (const auto& [pad_id, revision] : local_revisions_) {
    const auto ack = acked_revisions_.find(pad_id);
    if (ack == acked_revisions_.end() || ack->second < revision) return false;
  }
  return true;
}

}  // namespace kiko
