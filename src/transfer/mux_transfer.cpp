#include "transfer.hpp"

#include "core/compression.hpp"
#include "mux_receive_session.hpp"
#include "mux_send_scheduler.hpp"
#include "transfer_file_session.hpp"
#include "transfer_stream.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <span>

namespace kiko {

using namespace detail;

namespace {

void put_u64(Bytes& out, std::uint64_t value) {
  for (int i = 7; i >= 0; --i) out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xff));
}

void put_u32(Bytes& out, std::uint32_t value) {
  for (int i = 3; i >= 0; --i) out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xff));
}

}  // namespace

void send_files_mux(std::vector<TcpSocket>& channels, const SessionKey& key, const std::vector<FileEntry>& files,
                    ProgressReporter& reporter) {
  if (channels.empty()) throw KikoError("mux send requires at least one channel");
  const std::size_t n = channels.size();
  std::vector<StreamCipher> ciphers;
  ciphers.reserve(n);
  for (std::size_t k = 0; k < n; ++k) ciphers.emplace_back(key, true, static_cast<std::uint8_t>(k));

  Bytes buffer(kMuxChunk);
  std::uint64_t grand_total = 0;
  TransferTiming timing;
  timing.mode = "mux_send";
  timing.mux_channels = n;
  send_transfer_manifest(channels[0], ciphers[0], files);

  for (const auto& entry : files) {
    SendFileSession file(entry, channels[0], ciphers[0], reporter, timing, buffer);
    const auto action = file.action();
    if (action == SendFileAction::MarkerComplete) continue;
    if (action == SendFileAction::SkipPayload) {
      for (std::size_t k = 0; k < n; ++k) {
        send_tagged_timed(channels[k], ciphers[k], StreamTag::ChunkEnd, std::span<const std::uint8_t>(), timing);
      }
      grand_total += file.complete_skipped();
      continue;
    }

    MuxSendScheduler scheduler(channels, ciphers, reporter, timing);
    while (auto chunk = file.read_next(buffer)) {
      Bytes payload;
      payload.reserve(12 + chunk->bytes.size());
      put_u64(payload, chunk->offset);
      put_u32(payload, static_cast<std::uint32_t>(chunk->bytes.size()));
      if (file.use_zstd()) {
        const auto compress_start = TransferClock::now();
        auto compressed = zstd_compress_block(chunk->bytes);
        add_transfer_elapsed(timing.compress_ms, compress_start);
        payload.insert(payload.end(), compressed.begin(), compressed.end());
      } else {
        payload.insert(payload.end(), chunk->bytes.begin(), chunk->bytes.end());
      }
      scheduler.enqueue(std::move(payload), chunk->bytes.size());
    }

    scheduler.finish();
    grand_total += file.complete();
  }

  send_tagged_timed(channels[0], ciphers[0], StreamTag::Done, std::span<const std::uint8_t>(), timing);
  auto ack = recv_tagged(channels[0], ciphers[0]);
  if (!ack || ack->tag != StreamTag::Ack) throw KikoError("expected transfer ack");
  reporter.transfer_timing(timing);
  reporter.transfer_complete(files.size(), grand_total);
}

void receive_files_mux(std::vector<TcpSocket>& channels, const SessionKey& key, const std::filesystem::path& output_dir,
                       ProgressReporter& reporter, ConflictPolicy conflict_policy) {
  if (channels.empty()) throw KikoError("mux receive requires at least one channel");
  const std::size_t n = channels.size();
  std::vector<StreamCipher> ciphers;
  ciphers.reserve(n);
  for (std::size_t k = 0; k < n; ++k) ciphers.emplace_back(key, false, static_cast<std::uint8_t>(k));

  std::size_t file_count = 0;
  std::uint64_t grand_total = 0;
  TransferTiming timing;
  timing.mode = "mux_receive";
  timing.mux_channels = n;
  bool saw_manifest = false;
  bool saw_file_header = false;
  std::optional<ReceivePlan> receive_plan;
  Bytes file_buffer(kMuxChunk);

  while (true) {
    auto frame = recv_tagged(channels[0], ciphers[0]);
    if (!frame) throw KikoError("transfer stream ended unexpectedly");
    if (frame->tag == StreamTag::Manifest) {
      if (saw_file_header) throw KikoError("transfer manifest arrived after file headers");
      if (saw_manifest) throw KikoError("duplicate transfer manifest");
      const auto text = std::string(frame->payload.begin(), frame->payload.end());
      receive_plan = preflight_transfer_manifest(decode_transfer_manifest(text), output_dir, conflict_policy, reporter);
      saw_manifest = true;
      continue;
    }
    if (frame->tag == StreamTag::Done) {
      send_tagged_timed(channels[0], ciphers[0], StreamTag::Ack, std::span<const std::uint8_t>(), timing);
      reporter.transfer_timing(timing);
      reporter.transfer_complete(file_count, grand_total);
      return;
    }
    if (frame->tag != StreamTag::FileHeader) throw KikoError("expected file header on control channel");
    saw_file_header = true;

    auto header = decode_message(std::string(frame->payload.begin(), frame->payload.end()));
    ReceiveFileSession file(std::move(header), output_dir, conflict_policy,
                            receive_plan ? &*receive_plan : nullptr, channels[0], ciphers[0], reporter, file_buffer);
    const auto action = file.action();

    if (action == ReceiveFileAction::Marker) {
      auto endframe = recv_tagged(channels[0], ciphers[0]);
      if (!endframe || endframe->tag != StreamTag::FileEnd) throw KikoError("expected file end");
      file.complete_marker();
      ++file_count;
      continue;
    }

    if (action == ReceiveFileAction::SkipPayload) {
      for (std::size_t k = 0; k < n; ++k) {
        auto chunk_end = recv_tagged(channels[k], ciphers[k]);
        if (!chunk_end || chunk_end->tag != StreamTag::ChunkEnd) throw KikoError("expected mux chunk end");
      }
      auto endframe = recv_tagged(channels[0], ciphers[0]);
      if (!endframe || endframe->tag != StreamTag::FileEnd) throw KikoError("expected file end");
      ++file_count;
      grand_total += file.complete_skipped();
      continue;
    }

    if (file.resume_offset() == 0) {
      std::ofstream create(file.part_path(), std::ios::binary | std::ios::trunc);
    }
    std::fstream out(file.part_path(), std::ios::binary | std::ios::in | std::ios::out);
    if (!out) throw KikoError("failed to open output file: " + file.part_path().string());

    MuxReceiveSession receive_session(channels, ciphers, out, file.part_path(), file.relative(), file.declared_size(),
                                      file.resume_offset(), file.use_zstd(), reporter, timing);
    const auto written = receive_session.receive();

    auto endframe = recv_tagged(channels[0], ciphers[0]);
    if (!endframe || endframe->tag != StreamTag::FileEnd) throw KikoError("expected file end");
    auto trailer = decode_message(std::string(endframe->payload.begin(), endframe->payload.end()));
    const auto expected = trailer.get("sha256");
    out.flush();
    out.close();
    if (written > file.declared_size()) throw KikoError("received more data than declared for " + file.relative());
    ++file_count;
    grand_total += file.complete_received(expected, written, {}, file_buffer);
  }
}

}  // namespace kiko
