#include "note_protocol.hpp"

#include "core/common.hpp"
#include "core/protocol.hpp"

#include <utility>

namespace kiko {
namespace {

std::string encode_note_frame(const NoteFrame& frame) {
  if (frame.text.size() > kNoteMaxBytes) throw KikoError("note text exceeds 1 MiB limit");
  Message msg{"note",
              {{"kind", note_frame_type_name(frame.type)},
               {"revision", std::to_string(frame.revision)},
               {"timestamp_ms", std::to_string(frame.timestamp_ms)},
               {"text", frame.text}}};
  return encode_message(msg);
}

NoteFrame decode_note_frame(const std::string& payload) {
  if (payload.size() > kNoteMaxBytes + 256) throw KikoError("note frame exceeds size limit");
  auto msg = decode_message(payload);
  if (msg.type != "note") throw KikoError("unexpected note frame: " + msg.type);
  NoteFrame frame;
  frame.type = parse_note_frame_type(msg.get("kind"));
  frame.revision = msg.get_u64("revision", 0);
  frame.timestamp_ms = msg.get_u64("timestamp_ms", 0);
  frame.text = msg.get("text");
  if (frame.text.size() > kNoteMaxBytes) throw KikoError("note text exceeds 1 MiB limit");
  return frame;
}

}  // namespace

std::string note_frame_type_name(NoteFrameType type) {
  switch (type) {
    case NoteFrameType::Hello:
      return "hello";
    case NoteFrameType::Update:
      return "update";
    case NoteFrameType::Clear:
      return "clear";
    case NoteFrameType::Ack:
      return "ack";
    case NoteFrameType::Ping:
      return "ping";
    case NoteFrameType::Bye:
      return "bye";
  }
  return "update";
}

NoteFrameType parse_note_frame_type(const std::string& value) {
  if (value == "hello") return NoteFrameType::Hello;
  if (value == "update") return NoteFrameType::Update;
  if (value == "clear") return NoteFrameType::Clear;
  if (value == "ack") return NoteFrameType::Ack;
  if (value == "ping") return NoteFrameType::Ping;
  if (value == "bye") return NoteFrameType::Bye;
  throw KikoError("unknown note frame kind: " + value);
}

NoteFrame make_note_hello() {
  NoteFrame frame;
  frame.type = NoteFrameType::Hello;
  frame.timestamp_ms = now_ms();
  return frame;
}

NoteFrame make_note_update(std::uint64_t revision, std::string text) {
  if (text.size() > kNoteMaxBytes) throw KikoError("note text exceeds 1 MiB limit");
  NoteFrame frame;
  frame.type = NoteFrameType::Update;
  frame.revision = revision;
  frame.timestamp_ms = now_ms();
  frame.text = std::move(text);
  return frame;
}

NoteFrame make_note_clear(std::uint64_t revision) {
  auto frame = make_note_update(revision, {});
  frame.type = NoteFrameType::Clear;
  return frame;
}

NoteFrame make_note_ack(std::uint64_t revision) {
  NoteFrame frame;
  frame.type = NoteFrameType::Ack;
  frame.revision = revision;
  frame.timestamp_ms = now_ms();
  return frame;
}

bool apply_note_update(NoteDocument& document, const NoteFrame& frame) {
  if (frame.type != NoteFrameType::Update && frame.type != NoteFrameType::Clear) return false;
  if (frame.revision < document.revision) return false;
  if (frame.revision == document.revision && frame.timestamp_ms <= document.timestamp_ms) return false;
  document.revision = frame.revision;
  document.timestamp_ms = frame.timestamp_ms;
  document.text = frame.type == NoteFrameType::Clear ? std::string{} : frame.text;
  return true;
}

void send_note_frame(TcpSocket& socket, StreamCipher& cipher, const NoteFrame& frame) {
  const auto payload = encode_note_frame(frame);
  const auto bytes = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(payload.data()),
                                                  payload.size());
  send_frame(socket, cipher.encrypt(bytes));
}

std::optional<NoteFrame> recv_note_frame(TcpSocket& socket, StreamCipher& cipher) {
  auto encrypted = recv_frame(socket);
  if (!encrypted) return std::nullopt;
  auto plain = cipher.decrypt(*encrypted);
  return decode_note_frame(std::string(plain.begin(), plain.end()));
}

}  // namespace kiko
