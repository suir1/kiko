#pragma once

#include "transfer_stream.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace kiko::detail {

class MuxSendScheduler {
 public:
  MuxSendScheduler(std::vector<TcpSocket>& channels, std::vector<StreamCipher>& ciphers, ProgressReporter& reporter,
                   TransferTiming& timing);

  MuxSendScheduler(const MuxSendScheduler&) = delete;
  MuxSendScheduler& operator=(const MuxSendScheduler&) = delete;

  ~MuxSendScheduler();

  void enqueue(Bytes payload, std::uint64_t raw_size);
  void finish();

 private:
  struct SendItem {
    Bytes payload;
    std::uint64_t raw_size = 0;
  };

  struct ChannelState {
    std::uint64_t pending_bytes = 0;
    double send_ewma_ms = 0.0;
  };

  static constexpr std::uint64_t kMuxPendingChunksPerChannel = 4;
  static constexpr std::uint64_t kMaxPendingPerChannel = kMuxPendingChunksPerChannel * kMuxChunk;
  static constexpr double kLatencyPenaltyBytesPerMs = 16.0 * 1024.0;
  static constexpr double kSendEwmaAlpha = 0.2;

  [[nodiscard]] std::uint64_t per_channel_pending_limit(std::uint64_t payload_size) const;
  [[nodiscard]] std::uint64_t global_pending_limit(std::uint64_t payload_size) const;
  [[nodiscard]] std::uint64_t total_pending_locked() const;
  [[nodiscard]] double channel_cost_locked(const ChannelState& state) const;
  [[nodiscard]] std::optional<std::size_t> choose_ready_channel_locked(std::uint64_t payload_size);

  void record_channel_latency_locked(std::size_t channel_index, std::int64_t elapsed_ms);
  void record_pending_peak_locked();
  void record_backpressure_wait(std::int64_t elapsed_ms);
  void record_send_timing(std::int64_t elapsed_ms);
  void set_error(std::exception_ptr error);
  void worker(std::size_t channel_index);

  std::vector<TcpSocket>& channels_;
  std::vector<StreamCipher>& ciphers_;
  ProgressReporter& reporter_;
  TransferTiming& timing_;
  std::vector<std::deque<SendItem>> queues_;
  std::vector<ChannelState> channel_state_;
  std::vector<std::thread> workers_;
  std::mutex mutex_;
  std::mutex report_mutex_;
  std::mutex timing_mutex_;
  std::condition_variable cv_;
  std::exception_ptr error_;
  std::size_t next_channel_ = 0;
  bool finishing_ = false;
  bool joined_ = false;
};

}  // namespace kiko::detail
