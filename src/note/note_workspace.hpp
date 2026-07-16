#pragma once

#include "note/note_protocol.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace kiko {

struct NoteWorkspaceSnapshot {
  std::string active_pad = "main";
  std::vector<NoteDocument> documents;
  std::uint64_t latest_local_revision = 0;
  std::uint64_t last_acked_revision = 0;
  bool has_local_updates = false;
  bool synced = false;
};

class NoteWorkspace {
 public:
  NoteWorkspace();

  [[nodiscard]] NoteFrame update_active(std::string text);
  [[nodiscard]] NoteFrame clear_active();
  [[nodiscard]] NoteFrame create_pad();
  [[nodiscard]] bool select_pad(const std::string& pad_id);

  [[nodiscard]] bool apply_remote(const NoteFrame& frame);
  void acknowledge(const NoteFrame& frame);

  [[nodiscard]] NoteDocument active_document() const;
  [[nodiscard]] std::optional<NoteDocument> document(const std::string& pad_id) const;
  [[nodiscard]] NoteWorkspaceSnapshot snapshot() const;

 private:
  [[nodiscard]] NoteFrame apply_local_locked(NoteDocument& document, NoteFrame frame);
  NoteDocument& ensure_pad_locked(const std::string& pad_id, const std::string& title = {});
  [[nodiscard]] bool synced_locked() const;

  mutable std::mutex mutex_;
  std::map<std::string, NoteDocument> documents_;
  std::map<std::string, std::uint64_t> local_revisions_;
  std::map<std::string, std::uint64_t> acked_revisions_;
  std::string active_pad_ = "main";
  int next_pad_number_ = 2;
};

}  // namespace kiko
