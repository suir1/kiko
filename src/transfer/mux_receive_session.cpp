#include "mux_receive_session.hpp"

#include "core/compression.hpp"

#include <algorithm>
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
                                     std::fstream& output, std::filesystem::path part_path,
                                     std::string current_relative, std::uint64_t declared_size,
                                     std::uint64_t resume_offset, bool use_zstd, ProgressReporter& reporter,
                                     TransferTiming& timing)
    : channels_(channels),
      ciphers_(ciphers),
      output_(output),
      part_path_(std::move(part_path)),
      current_relative_(std::move(current_relative)),
      declared_size_(declared_size),
      resume_offset_(resume_offset),
      use_zstd_(use_zstd),
      reporter_(reporter),
      timing_(timing),
      written_(resume_offset),
      contiguous_prefix_(resume_offset) {}

std::uint64_t MuxReceiveSession::receive() {
  std::vector<std::thread> threads;
  threads.reserve(channels_.size() > 0 ? channels_.size() - 1 : 0);
  for (std::size_t k = 1; k < channels_.size(); ++k) {
    threads.emplace_back([this, k] { reader(k); });
  }
  reader(0);
  for (auto& thread : threads) thread.join();
  if (failed_.load()) {
    truncate_partial_to_contiguous_prefix();
    throw KikoError("mux receive failed: " + error_text_);
  }
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

      std::uint64_t contiguous_delta = 0;
      std::int64_t write_ms = 0;
      {
        std::lock_guard<std::mutex> lock(file_mutex_);
        const auto write_start = TransferClock::now();
        output_.seekp(static_cast<std::streamoff>(offset));
        output_.write(reinterpret_cast<const char*>(plain.data()), static_cast<std::streamsize>(plain.size()));
        write_ms = transfer_elapsed_ms_since(write_start);
        if (!output_) throw KikoError("write failed during mux receive");
        contiguous_delta = record_written_range_locked(offset, static_cast<std::uint64_t>(plain.size()));
      }
      written_.fetch_add(plain.size());
      {
        std::lock_guard<std::mutex> timing_lock(timing_mutex_);
        timing_.decompress_ms += decompress_ms;
        timing_.disk_write_ms += write_ms;
        timing_.payload_bytes += plain.size();
      }
      if (contiguous_delta > 0) {
        std::lock_guard<std::mutex> report_lock(report_mutex_);
        reporter_.file_advance(contiguous_delta);
      }
    }
  } catch (const std::exception& error) {
    record_failure(error);
  }
}

std::uint64_t MuxReceiveSession::record_written_range_locked(std::uint64_t offset, std::uint64_t size) {
  if (size == 0) return 0;
  const auto previous_prefix = contiguous_prefix_;
  const auto end = offset + size;
  if (end <= contiguous_prefix_) return 0;

  if (offset <= contiguous_prefix_) {
    contiguous_prefix_ = std::max(contiguous_prefix_, end);
  } else {
    auto [it, inserted] = pending_ranges_.emplace(offset, end);
    if (!inserted && end > it->second) it->second = end;
  }

  while (!pending_ranges_.empty()) {
    auto it = pending_ranges_.begin();
    if (it->first > contiguous_prefix_) break;
    contiguous_prefix_ = std::max(contiguous_prefix_, it->second);
    pending_ranges_.erase(it);
  }
  return contiguous_prefix_ - previous_prefix;
}

void MuxReceiveSession::record_failure(const std::exception& error) {
  failed_.store(true);
  {
    std::lock_guard<std::mutex> report_lock(report_mutex_);
    if (error_text_.empty()) error_text_ = error.what();
  }
  close_channels();
}

void MuxReceiveSession::truncate_partial_to_contiguous_prefix() {
  std::error_code ec;
  {
    std::lock_guard<std::mutex> lock(file_mutex_);
    output_.flush();
    output_.close();
    std::filesystem::resize_file(part_path_, contiguous_prefix_, ec);
  }
  if (!ec) return;

  std::lock_guard<std::mutex> report_lock(report_mutex_);
  if (!error_text_.empty()) error_text_ += "; ";
  error_text_ += "failed to trim mux partial to resumable prefix: " + ec.message();
}

void MuxReceiveSession::close_channels() {
  bool expected = false;
  if (!closing_.compare_exchange_strong(expected, true)) return;
  for (auto& channel : channels_) channel.close();
}

}  // namespace kiko::detail
