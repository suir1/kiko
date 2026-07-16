#pragma once

#include "core/crypto.hpp"
#include "core/socket.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace kiko {

inline constexpr std::size_t kNoteMaxBytes = 1024 * 1024;

enum class NoteFrameType {
  Hello,
  Update,
  Clear,
  Ack,
  Ping,
  Bye,
};

struct NoteFrame {
  NoteFrameType type = NoteFrameType::Update;
  std::uint64_t revision = 0;
  std::uint64_t timestamp_ms = 0;
  std::string pad_id = "main";
  std::string title;
  std::string text;
};

struct NoteDocument {
  std::uint64_t revision = 0;
  std::uint64_t timestamp_ms = 0;
  std::string pad_id = "main";
  std::string title;
  std::string text;
};

[[nodiscard]] NoteFrame make_note_hello();
[[nodiscard]] NoteFrame make_note_update(std::uint64_t revision, std::string text);
[[nodiscard]] NoteFrame make_note_update(std::string pad_id, std::uint64_t revision, std::string text,
                                         std::string title = {});
[[nodiscard]] NoteFrame make_note_clear(std::uint64_t revision);
[[nodiscard]] NoteFrame make_note_clear(std::string pad_id, std::uint64_t revision, std::string title = {});
[[nodiscard]] NoteFrame make_note_ack(std::uint64_t revision);
[[nodiscard]] NoteFrame make_note_ack(std::string pad_id, std::uint64_t revision);
[[nodiscard]] bool apply_note_update(NoteDocument& document, const NoteFrame& frame);

void send_note_frame(TcpSocket& socket, StreamCipher& cipher, const NoteFrame& frame);
[[nodiscard]] std::optional<NoteFrame> recv_note_frame_timeout(TcpSocket& socket, StreamCipher& cipher,
                                                               std::chrono::milliseconds timeout,
                                                               const std::atomic_bool* cancel = nullptr);

}  // namespace kiko
