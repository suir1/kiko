#pragma once

#include "transfer_stream.hpp"

#include <atomic>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace kiko::detail {

class MuxReceiveSession {
 public:
  MuxReceiveSession(std::vector<TcpSocket>& channels, std::vector<StreamCipher>& ciphers, std::fstream& output,
                    std::filesystem::path part_path, std::string current_relative, std::uint64_t declared_size,
                    std::uint64_t resume_offset, bool use_zstd, ProgressReporter& reporter, TransferTiming& timing);

  [[nodiscard]] std::uint64_t receive();

 private:
  void reader(std::size_t channel_index);
  [[nodiscard]] std::uint64_t record_written_range_locked(std::uint64_t offset, std::uint64_t size);
  void record_failure(const std::exception& error);
  void truncate_partial_to_contiguous_prefix();
  void interrupt_channels();

  std::vector<TcpSocket>& channels_;
  std::vector<StreamCipher>& ciphers_;
  std::fstream& output_;
  std::filesystem::path part_path_;
  std::string current_relative_;
  std::uint64_t declared_size_ = 0;
  std::uint64_t resume_offset_ = 0;
  bool use_zstd_ = false;
  ProgressReporter& reporter_;
  TransferTiming& timing_;
  std::mutex file_mutex_;
  std::mutex report_mutex_;
  std::mutex timing_mutex_;
  std::atomic<std::uint64_t> written_;
  std::uint64_t contiguous_prefix_ = 0;
  std::map<std::uint64_t, std::uint64_t> pending_ranges_;
  std::atomic<bool> failed_{false};
  std::atomic<bool> interrupting_{false};
  std::string error_text_;
};

}  // namespace kiko::detail
