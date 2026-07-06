#pragma once

#include "core/crypto.hpp"
#include "core/socket.hpp"

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
  std::string text;
};

struct NoteDocument {
  std::uint64_t revision = 0;
  std::uint64_t timestamp_ms = 0;
  std::string text;
};

[[nodiscard]] std::string note_frame_type_name(NoteFrameType type);
[[nodiscard]] NoteFrameType parse_note_frame_type(const std::string& value);

[[nodiscard]] NoteFrame make_note_hello();
[[nodiscard]] NoteFrame make_note_update(std::uint64_t revision, std::string text);
[[nodiscard]] NoteFrame make_note_clear(std::uint64_t revision);
[[nodiscard]] NoteFrame make_note_ack(std::uint64_t revision);
[[nodiscard]] bool apply_note_update(NoteDocument& document, const NoteFrame& frame);

void send_note_frame(TcpSocket& socket, StreamCipher& cipher, const NoteFrame& frame);
[[nodiscard]] std::optional<NoteFrame> recv_note_frame(TcpSocket& socket, StreamCipher& cipher);

}  // namespace kiko
