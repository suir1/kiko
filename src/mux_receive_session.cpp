#include "mux_receive_session.hpp"

#include "compression.hpp"

#include <span>
#include <thread>

namespace kiko::detail {
namespace {

std::uint64_t read_u64(const std::uint8_t* p) {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
  return v;
}

std::uint32_t read_u32(const std::uint8_t* p) {
  std::uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v = (v << 8) | p[i];
  return v;
}

}  // namespace

MuxReceiveSession::MuxReceiveSession(std::vector<TcpSocket>& channels, std::vector<StreamCipher>& ciphers,
                                     std::fstream& output, std::string current_relative,
                                     std::uint64_t declared_size, std::uint64_t resume_offset, bool use_zstd,
                                     ProgressReporter& reporter, TransferTiming& timing)
    : channels_(channels),
      ciphers_(ciphers),
      output_(output),
      current_relative_(std::move(current_relative)),
      declared_size_(declared_size),
      resume_offset_(resume_offset),
      use_zstd_(use_zstd),
      reporter_(reporter),
      timing_(timing),
      written_(resume_offset) {}

std::uint64_t MuxReceiveSession::receive() {
  std::vector<std::thread> threads;
  threads.reserve(channels_.size() > 0 ? channels_.size() - 1 : 0);
  for (std::size_t k = 1; k < channels_.size(); ++k) {
    threads.emplace_back([this, k] { reader(k); });
  }
  reader(0);
  for (auto& thread : threads) thread.join();
  if (failed_.load()) throw KikoError("mux receive failed: " + error_text_);
  return written_.load();
}

void MuxReceiveSession::reader(std::size_t channel_index) {
  try {
    while (true) {
      auto frame = recv_tagged(channels_[channel_index], ciphers_[channel_index]);
      if (!frame) throw KikoError("data channel closed early");
      if (frame->tag == StreamTag::ChunkEnd) break;
      if (frame->tag != StreamTag::Data) throw KikoError("unexpected frame on data channel");
      if (frame->payload.size() < 12) throw KikoError("short data frame");

      const std::uint64_t offset = read_u64(frame->payload.data());
      const std::uint32_t raw_len = read_u32(frame->payload.data() + 8);
      if (raw_len > kMuxChunk) throw KikoError("mux data chunk too large for " + current_relative_);
      if (offset < resume_offset_) throw KikoError("mux data before resume offset for " + current_relative_);
      ensure_declared_space(offset, declared_size_, static_cast<std::uint64_t>(raw_len), current_relative_);

      std::span<const std::uint8_t> compressed(frame->payload.data() + 12, frame->payload.size() - 12);
      Bytes plain;
      std::int64_t decompress_ms = 0;
      if (use_zstd_) {
        const auto decompress_start = TransferClock::now();
        plain = zstd_decompress_block(compressed, raw_len);
        decompress_ms = transfer_elapsed_ms_since(decompress_start);
      } else {
        if (compressed.size() != raw_len) throw KikoError("uncompressed chunk size mismatch");
        plain.assign(compressed.begin(), compressed.end());
      }
      if (plain.size() != raw_len) throw KikoError("mux decoded chunk size mismatch");
      ensure_declared_space(offset, declared_size_, static_cast<std::uint64_t>(plain.size()), current_relative_);

      std::int64_t write_ms = 0;
      {
        std::lock_guard<std::mutex> lock(file_mutex_);
        const auto write_start = TransferClock::now();
        output_.seekp(static_cast<std::streamoff>(offset));
        output_.write(reinterpret_cast<const char*>(plain.data()), static_cast<std::streamsize>(plain.size()));
        write_ms = transfer_elapsed_ms_since(write_start);
        if (!output_) throw KikoError("write failed during mux receive");
      }
      written_.fetch_add(plain.size());
      {
        std::lock_guard<std::mutex> timing_lock(timing_mutex_);
        timing_.decompress_ms += decompress_ms;
        timing_.disk_write_ms += write_ms;
        timing_.payload_bytes += plain.size();
      }
      std::lock_guard<std::mutex> report_lock(report_mutex_);
      reporter_.file_advance(plain.size());
    }
  } catch (const std::exception& error) {
    record_failure(error);
  }
}

void MuxReceiveSession::record_failure(const std::exception& error) {
  failed_.store(true);
  std::lock_guard<std::mutex> report_lock(report_mutex_);
  if (error_text_.empty()) error_text_ = error.what();
}

}  // namespace kiko::detail
