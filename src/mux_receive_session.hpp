#pragma once

#include "transfer_stream.hpp"

#include <atomic>
#include <cstdint>
#include <exception>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace kiko::detail {

class MuxReceiveSession {
 public:
  MuxReceiveSession(std::vector<TcpSocket>& channels, std::vector<StreamCipher>& ciphers, std::fstream& output,
                    std::string current_relative, std::uint64_t declared_size, std::uint64_t resume_offset,
                    bool use_zstd, ProgressReporter& reporter, TransferTiming& timing);

  [[nodiscard]] std::uint64_t receive();

 private:
  void reader(std::size_t channel_index);
  void record_failure(const std::exception& error);
  void close_channels();

  std::vector<TcpSocket>& channels_;
  std::vector<StreamCipher>& ciphers_;
  std::fstream& output_;
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
  std::atomic<bool> failed_{false};
  std::atomic<bool> closing_{false};
  std::string error_text_;
};

}  // namespace kiko::detail
