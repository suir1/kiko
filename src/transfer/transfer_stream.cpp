#include "transfer_stream.hpp"

#include "core/compression.hpp"
#include "transfer_file_session.hpp"
#include "transfer_manifest.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <limits>
#include <memory>

namespace kiko::detail {

void ensure_declared_space(std::uint64_t current_total, std::uint64_t declared_size, std::uint64_t next_size,
                           const std::string& relative) {
  if (current_total > declared_size || next_size > declared_size - current_total) {
    throw KikoError("received more data than declared for " + relative);
  }
}

std::size_t declared_remaining_limit(std::uint64_t current_total, std::uint64_t declared_size,
                                     const std::string& relative) {
  ensure_declared_space(current_total, declared_size, 0, relative);
  const auto remaining = declared_size - current_total;
  const auto max_size = static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
  return remaining > max_size ? std::numeric_limits<std::size_t>::max() : static_cast<std::size_t>(remaining);
}

void send_transfer_manifest(TcpSocket& socket, StreamCipher& cipher, const std::vector<FileEntry>& files) {
  send_tagged_text(socket, cipher, StreamTag::Manifest, encode_transfer_manifest(files));
}

void send_tagged(TcpSocket& socket, StreamCipher& cipher, StreamTag tag, std::span<const std::uint8_t> payload) {
  Bytes plain;
  plain.reserve(payload.size() + 1);
  plain.push_back(static_cast<std::uint8_t>(tag));
  plain.insert(plain.end(), payload.begin(), payload.end());
  send_frame(socket, cipher.encrypt(plain));
}

void send_tagged_text(TcpSocket& socket, StreamCipher& cipher, StreamTag tag, const std::string& text) {
  send_tagged(socket, cipher, tag,
              std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

std::optional<TaggedFrame> recv_tagged(TcpSocket& socket, StreamCipher& cipher) {
  auto encrypted = recv_frame(socket);
  if (!encrypted) return std::nullopt;
  auto plain = cipher.decrypt(*encrypted);
  if (plain.empty()) throw KikoError("malformed transfer frame");
  TaggedFrame frame;
  frame.tag = static_cast<StreamTag>(plain[0]);
  frame.payload.assign(plain.begin() + 1, plain.end());
  return frame;
}

void add_transfer_elapsed(std::int64_t& bucket, TransferClock::time_point start) {
  bucket += elapsed_ms_since(start);
}

void send_tagged_timed(TcpSocket& socket, StreamCipher& cipher, StreamTag tag,
                       std::span<const std::uint8_t> payload, TransferTiming& timing) {
  const auto start = TransferClock::now();
  send_tagged(socket, cipher, tag, payload);
  const auto elapsed = elapsed_ms_since(start);
  timing.send_frame_ms += elapsed;
  timing.max_send_frame_ms = std::max(timing.max_send_frame_ms, static_cast<int>(elapsed));
  ++timing.frame_count;
}

void send_tagged_text_timed(TcpSocket& socket, StreamCipher& cipher, StreamTag tag, const std::string& text,
                            TransferTiming& timing) {
  send_tagged_timed(socket, cipher, tag,
                    std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()),
                    timing);
}

}  // namespace kiko::detail

namespace kiko {

using namespace detail;

void send_files(TcpSocket& channel, const SessionKey& key, const std::vector<FileEntry>& files,
                ProgressReporter& reporter) {
  StreamCipher cipher(key, /*sender_originates=*/true);
  Bytes buffer(kPlainChunk);
  std::uint64_t grand_total = 0;
  TransferTiming timing;
  timing.mode = "stream_send";
  send_transfer_manifest(channel, cipher, files);

  for (const auto& entry : files) {
    SendFileSession file(entry, channel, cipher, reporter, timing, buffer);
    const auto action = file.action();
    if (action == SendFileAction::MarkerComplete) continue;
    if (action == SendFileAction::SkipPayload) {
      grand_total += file.complete_skipped();
      continue;
    }

    std::optional<ZstdStreamCompressor> compressor;
    if (file.use_zstd()) compressor.emplace(3);
    while (auto chunk = file.read_next(buffer)) {
      if (file.use_zstd()) {
        const auto compress_start = TransferClock::now();
        auto compressed = compressor->compress(chunk->bytes, false);
        add_transfer_elapsed(timing.compress_ms, compress_start);
        if (!compressed.empty()) send_tagged_timed(channel, cipher, StreamTag::Data, compressed, timing);
      } else {
        send_tagged_timed(channel, cipher, StreamTag::Data, chunk->bytes, timing);
      }
      reporter.file_advance(chunk->bytes.size());
    }
    if (file.use_zstd()) {
      const auto compress_start = TransferClock::now();
      auto trailer = compressor->compress(std::span<const std::uint8_t>(), true);
      add_transfer_elapsed(timing.compress_ms, compress_start);
      if (!trailer.empty()) send_tagged_timed(channel, cipher, StreamTag::Data, trailer, timing);
    }

    grand_total += file.complete();
  }

  send_tagged_timed(channel, cipher, StreamTag::Done, std::span<const std::uint8_t>(), timing);
  auto ack = recv_tagged(channel, cipher);
  if (!ack || ack->tag != StreamTag::Ack) throw KikoError("expected transfer ack");
  reporter.transfer_timing(timing);
  reporter.transfer_complete(files.size(), grand_total);
}

void receive_files(TcpSocket& channel, const SessionKey& key, const std::filesystem::path& output_dir,
                   ProgressReporter& reporter, ConflictPolicy conflict_policy) {
  StreamCipher cipher(key, /*sender_originates=*/false);
  Bytes buffer(kPlainChunk);
  TransferTiming timing;
  timing.mode = "stream_receive";

  std::ofstream out;
  std::optional<ZstdStreamDecompressor> decompressor;
  std::unique_ptr<ReceiveFileSession> current_file;
  std::uint64_t current_total = 0;
  std::size_t file_count = 0;
  std::uint64_t grand_total = 0;
  bool saw_manifest = false;
  bool saw_file_header = false;
  std::optional<ReceivePlan> receive_plan;

  while (true) {
    auto frame = recv_tagged(channel, cipher);
    if (!frame) throw KikoError("transfer stream ended unexpectedly");

    switch (frame->tag) {
      case StreamTag::Manifest: {
        if (saw_file_header) throw KikoError("transfer manifest arrived after file headers");
        if (saw_manifest) throw KikoError("duplicate transfer manifest");
        const auto text = std::string(frame->payload.begin(), frame->payload.end());
        receive_plan = preflight_transfer_manifest(decode_transfer_manifest(text), output_dir, conflict_policy, reporter);
        saw_manifest = true;
        break;
      }
      case StreamTag::FileHeader: {
        if (current_file) throw KikoError("file header before previous file end");
        saw_file_header = true;
        auto header = decode_message(std::string(frame->payload.begin(), frame->payload.end()));
        auto file = std::make_unique<ReceiveFileSession>(
            std::move(header), output_dir, conflict_policy, receive_plan ? &*receive_plan : nullptr, channel, cipher,
            reporter, buffer);
        const auto action = file->action();
        current_total = action == ReceiveFileAction::ReceivePayload ? file->resume_offset() : file->declared_size();
        decompressor.reset();
        if (action == ReceiveFileAction::ReceivePayload) {
          out = std::ofstream(file->part_path(),
                              std::ios::binary | (file->resume_offset() > 0 ? std::ios::app : std::ios::trunc));
          if (!out) throw KikoError("failed to open output file: " + file->part_path().string());
          if (file->use_zstd()) decompressor.emplace();
        }
        current_file = std::move(file);
        break;
      }
      case StreamTag::Data: {
        if (!current_file) throw KikoError("data frame before file header");
        if (current_file->action() == ReceiveFileAction::SkipPayload) break;
        if (current_file->action() != ReceiveFileAction::ReceivePayload) {
          throw KikoError("data frame for non-file entry");
        }
        if (current_file->use_zstd()) {
          if (!decompressor) throw KikoError("data frame before decompressor ready");
          const auto decompress_start = TransferClock::now();
          auto decompressed = decompressor->decompress(
              frame->payload,
              declared_remaining_limit(current_total, current_file->declared_size(), current_file->relative()));
          add_transfer_elapsed(timing.decompress_ms, decompress_start);
          if (!decompressed.empty()) {
            const auto write_start = TransferClock::now();
            out.write(reinterpret_cast<const char*>(decompressed.data()),
                      static_cast<std::streamsize>(decompressed.size()));
            add_transfer_elapsed(timing.disk_write_ms, write_start);
            const auto hash_start = TransferClock::now();
            current_file->hasher().update(decompressed);
            add_transfer_elapsed(timing.hash_ms, hash_start);
            current_total += decompressed.size();
            timing.payload_bytes += decompressed.size();
            reporter.file_advance(decompressed.size());
          }
        } else {
          if (!frame->payload.empty()) {
            ensure_declared_space(current_total, current_file->declared_size(),
                                  static_cast<std::uint64_t>(frame->payload.size()), current_file->relative());
            const auto write_start = TransferClock::now();
            out.write(reinterpret_cast<const char*>(frame->payload.data()),
                      static_cast<std::streamsize>(frame->payload.size()));
            add_transfer_elapsed(timing.disk_write_ms, write_start);
            const auto hash_start = TransferClock::now();
            current_file->hasher().update(frame->payload);
            add_transfer_elapsed(timing.hash_ms, hash_start);
            current_total += frame->payload.size();
            timing.payload_bytes += frame->payload.size();
            reporter.file_advance(frame->payload.size());
          }
        }
        break;
      }
      case StreamTag::FileEnd: {
        if (!current_file) throw KikoError("file end before file header");
        if (current_file->action() == ReceiveFileAction::Marker) {
          current_file->complete_marker();
          ++file_count;
        } else if (current_file->action() == ReceiveFileAction::SkipPayload) {
          ++file_count;
          grand_total += current_file->complete_skipped();
        } else {
          out.flush();
          out.close();
          auto trailer = decode_message(std::string(frame->payload.begin(), frame->payload.end()));
          const auto expected = trailer.get("sha256");
          const auto actual = hex_encode(current_file->hasher().finish());
          grand_total += current_file->complete_received(expected, current_total, actual, buffer);
          ++file_count;
        }
        current_file.reset();
        current_total = 0;
        decompressor.reset();
        break;
      }
      case StreamTag::Done: {
        if (current_file) throw KikoError("transfer done before file end");
        send_tagged_timed(channel, cipher, StreamTag::Ack, std::span<const std::uint8_t>(), timing);
        reporter.transfer_timing(timing);
        reporter.transfer_complete(file_count, grand_total);
        return;
      }
      default:
        throw KikoError("unknown transfer frame tag");
    }
  }
}

}  // namespace kiko
