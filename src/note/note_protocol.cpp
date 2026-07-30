#include "note_protocol.hpp"

#include "core/common.hpp"
#include "core/protocol.hpp"

#include <tuple>
#include <utility>

namespace kiko {
namespace {

constexpr std::pair<NoteFrameType, const char*> kNoteFrameTypes[] = {
    {NoteFrameType::Hello, "hello"},   {NoteFrameType::Update, "update"},
    {NoteFrameType::Clear, "clear"},   {NoteFrameType::Ack, "ack"},
    {NoteFrameType::Ping, "ping"},     {NoteFrameType::Bye, "bye"},
};
constexpr std::size_t kNoteCipherOverhead = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES +
                                             crypto_aead_xchacha20poly1305_ietf_ABYTES;

std::string note_frame_type_name(NoteFrameType type) {
  for (const auto& [candidate, name] : kNoteFrameTypes) {
    if (candidate == type) return name;
  }
  return "update";
}

NoteFrameType parse_note_frame_type(const std::string& value) {
  for (const auto& [type, name] : kNoteFrameTypes) {
    if (value == name) return type;
  }
  throw KikoError("unknown note frame kind: " + value);
}

void validate_note_frame_fields(const NoteFrame& frame) {
  if (frame.text.size() > kNoteMaxBytes) throw KikoError("note text exceeds 1 MiB limit");
  if (frame.pad_id.size() > kNoteMaxPadIdBytes) throw KikoError("note pad id exceeds 128 byte limit");
  if (frame.title.size() > kNoteMaxTitleBytes) throw KikoError("note title exceeds 256 byte limit");
  if (frame.writer_id.size() > kNoteMaxWriterIdBytes) throw KikoError("note writer id exceeds 128 byte limit");
}

}  // namespace

std::string encode_note_frame(const NoteFrame& frame) {
  validate_note_frame_fields(frame);
  Message msg{"note",
              {{"kind", note_frame_type_name(frame.type)},
               {"protocol_version", std::to_string(frame.protocol_version)},
               {"revision", std::to_string(frame.revision)},
               {"timestamp_ms", std::to_string(frame.timestamp_ms)},
               {"writer_id", frame.writer_id},
               {"pad_id", frame.pad_id.empty() ? "main" : frame.pad_id},
               {"title", frame.title},
               {"text", frame.text}}};
  auto payload = encode_message(msg);
  if (payload.size() > kNoteMaxWireBytes) throw KikoError("note frame exceeds wire size limit");
  return payload;
}

NoteFrame decode_note_frame(const std::string& payload) {
  if (payload.size() > kNoteMaxWireBytes) throw KikoError("note frame exceeds wire size limit");
  auto msg = decode_message(payload);
  if (msg.type != "note") throw KikoError("unexpected note frame: " + msg.type);
  NoteFrame frame;
  frame.type = parse_note_frame_type(msg.get("kind"));
  frame.protocol_version = msg.get_u64("protocol_version", 0);
  frame.revision = msg.get_u64("revision", 0);
  frame.timestamp_ms = msg.get_u64("timestamp_ms", 0);
  frame.writer_id = msg.get("writer_id");
  frame.pad_id = msg.get("pad_id", "main");
  if (frame.pad_id.empty()) frame.pad_id = "main";
  frame.title = msg.get("title");
  frame.text = msg.get("text");
  validate_note_frame_fields(frame);
  return frame;
}

NoteFrame make_note_hello() {
  NoteFrame frame;
  frame.type = NoteFrameType::Hello;
  frame.timestamp_ms = now_ms();
  return frame;
}

void validate_note_hello(const NoteFrame& frame) {
  if (frame.type != NoteFrameType::Hello) throw KikoError("note peer did not send hello");
  if (frame.protocol_version == kNoteProtocolVersion) return;
  const auto peer_version = frame.protocol_version == 0 ? std::string("legacy")
                                                        : std::to_string(frame.protocol_version);
  throw KikoError("incompatible notepad protocol (peer=" + peer_version +
                  ", local=" + std::to_string(kNoteProtocolVersion) + "); update kiko on both devices");
}

NoteFrame make_note_update(std::string pad_id, std::uint64_t revision, std::string text, std::string title,
                           std::string writer_id) {
  if (text.size() > kNoteMaxBytes) throw KikoError("note text exceeds 1 MiB limit");
  NoteFrame frame;
  frame.type = NoteFrameType::Update;
  frame.revision = revision;
  frame.timestamp_ms = now_ms();
  frame.writer_id = std::move(writer_id);
  frame.pad_id = pad_id.empty() ? "main" : std::move(pad_id);
  frame.title = std::move(title);
  frame.text = std::move(text);
  return frame;
}

NoteFrame make_note_clear(std::string pad_id, std::uint64_t revision, std::string title, std::string writer_id) {
  auto frame = make_note_update(std::move(pad_id), revision, {}, std::move(title), std::move(writer_id));
  frame.type = NoteFrameType::Clear;
  return frame;
}

NoteFrame make_note_ack(std::string pad_id, std::uint64_t revision) {
  NoteFrame frame;
  frame.type = NoteFrameType::Ack;
  frame.revision = revision;
  frame.timestamp_ms = now_ms();
  frame.pad_id = pad_id.empty() ? "main" : std::move(pad_id);
  return frame;
}

bool apply_note_update(NoteDocument& document, const NoteFrame& frame) {
  if (frame.type != NoteFrameType::Update && frame.type != NoteFrameType::Clear) return false;
  if (frame.revision < document.revision) return false;
  const auto incoming_text = frame.type == NoteFrameType::Clear ? std::string{} : frame.text;
  if (frame.revision == document.revision &&
      std::tie(frame.writer_id, frame.timestamp_ms, incoming_text) <=
          std::tie(document.writer_id, document.timestamp_ms, document.text)) {
    return false;
  }
  document.revision = frame.revision;
  document.timestamp_ms = frame.timestamp_ms;
  document.writer_id = frame.writer_id;
  document.pad_id = frame.pad_id.empty() ? "main" : frame.pad_id;
  if (!frame.title.empty()) document.title = frame.title;
  document.text = incoming_text;
  return true;
}

void send_note_frame(TcpSocket& socket, StreamCipher& cipher, const NoteFrame& frame) {
  const auto payload = encode_note_frame(frame);
  const auto bytes = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(payload.data()),
                                                  payload.size());
  send_frame(socket, cipher.encrypt(bytes));
}

std::optional<NoteFrame> recv_note_frame_timeout(TcpSocket& socket, StreamCipher& cipher,
                                                 std::chrono::milliseconds timeout,
                                                 const std::atomic_bool* cancel) {
  auto encrypted = recv_frame_timeout(socket, timeout, cancel, kNoteMaxWireBytes + kNoteCipherOverhead);
  if (!encrypted) return std::nullopt;
  auto plain = cipher.decrypt(*encrypted);
  return decode_note_frame(std::string(plain.begin(), plain.end()));
}

}  // namespace kiko
