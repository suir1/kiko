#include "mux_send_scheduler.hpp"

#include <algorithm>
#include <span>

namespace kiko::detail {

MuxSendScheduler::MuxSendScheduler(std::vector<TcpSocket>& channels, std::vector<StreamCipher>& ciphers,
                                   ProgressReporter& reporter, TransferTiming& timing)
    : channels_(channels),
      ciphers_(ciphers),
      reporter_(reporter),
      timing_(timing),
      queues_(channels.size()),
      channel_state_(channels.size()) {
  timing_.mux_channels = channels_.size();
  workers_.reserve(channels_.size());
  for (std::size_t k = 0; k < channels_.size(); ++k) workers_.emplace_back([this, k] { worker(k); });
}

MuxSendScheduler::~MuxSendScheduler() {
  if (!joined_) {
    try {
      finish();
    } catch (...) {
    }
  }
}

void MuxSendScheduler::enqueue(Bytes payload, std::uint64_t raw_size) {
  std::unique_lock<std::mutex> lock(mutex_);
  const auto payload_size = static_cast<std::uint64_t>(payload.size());
  auto channel_index = choose_ready_channel_locked(payload_size);
  if (!channel_index && !error_) {
    const auto wait_start = TransferClock::now();
    cv_.wait(lock, [&] {
      channel_index = choose_ready_channel_locked(payload_size);
      return error_ || channel_index.has_value();
    });
    if (!error_) record_backpressure_wait(transfer_elapsed_ms_since(wait_start));
  }
  if (error_) std::rethrow_exception(error_);

  channel_state_[*channel_index].pending_bytes += payload_size;
  queues_[*channel_index].push_back(SendItem{std::move(payload), raw_size});
  record_pending_peak_locked();
  cv_.notify_all();
}

void MuxSendScheduler::finish() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    finishing_ = true;
  }
  cv_.notify_all();
  for (auto& worker : workers_) {
    if (worker.joinable()) worker.join();
  }
  joined_ = true;
  if (error_) std::rethrow_exception(error_);
}

std::uint64_t MuxSendScheduler::per_channel_pending_limit(std::uint64_t payload_size) const {
  return std::max<std::uint64_t>(kMaxPendingPerChannel, payload_size);
}

std::uint64_t MuxSendScheduler::global_pending_limit(std::uint64_t payload_size) const {
  return std::max<std::uint64_t>(static_cast<std::uint64_t>(channel_state_.size()) * kMaxPendingPerChannel,
                                 payload_size);
}

std::uint64_t MuxSendScheduler::total_pending_locked() const {
  std::uint64_t total = 0;
  for (const auto& state : channel_state_) total += state.pending_bytes;
  return total;
}

double MuxSendScheduler::channel_cost_locked(const ChannelState& state) const {
  return static_cast<double>(state.pending_bytes) + state.send_ewma_ms * kLatencyPenaltyBytesPerMs;
}

std::optional<std::size_t> MuxSendScheduler::choose_ready_channel_locked(std::uint64_t payload_size) {
  if (channel_state_.empty()) return std::nullopt;
  const auto global_limit = global_pending_limit(payload_size);
  const auto total_pending = total_pending_locked();
  if (total_pending > global_limit - payload_size) return std::nullopt;

  const auto channel_limit = per_channel_pending_limit(payload_size);
  std::optional<std::size_t> best;
  double best_cost = 0.0;
  for (std::size_t step = 0; step < channel_state_.size(); ++step) {
    const auto k = (next_channel_ + step) % channel_state_.size();
    const auto& state = channel_state_[k];
    if (state.pending_bytes > channel_limit - payload_size) continue;
    const auto cost = channel_cost_locked(state);
    if (!best || cost < best_cost) {
      best = k;
      best_cost = cost;
    }
  }
  if (!best) return std::nullopt;
  next_channel_ = (*best + 1) % channel_state_.size();
  return best;
}

void MuxSendScheduler::record_channel_latency_locked(std::size_t channel_index, std::int64_t elapsed_ms) {
  auto& state = channel_state_[channel_index];
  const auto sample = static_cast<double>(std::max<std::int64_t>(0, elapsed_ms));
  if (state.send_ewma_ms <= 0.0) {
    state.send_ewma_ms = sample;
  } else {
    state.send_ewma_ms = state.send_ewma_ms * (1.0 - kSendEwmaAlpha) + sample * kSendEwmaAlpha;
  }
}

void MuxSendScheduler::record_pending_peak_locked() {
  std::lock_guard<std::mutex> timing_lock(timing_mutex_);
  timing_.mux_max_pending_bytes = std::max(timing_.mux_max_pending_bytes, total_pending_locked());
}

void MuxSendScheduler::record_backpressure_wait(std::int64_t elapsed_ms) {
  std::lock_guard<std::mutex> timing_lock(timing_mutex_);
  timing_.mux_backpressure_wait_ms += elapsed_ms;
  ++timing_.mux_backpressure_wait_count;
}

void MuxSendScheduler::record_send_timing(std::int64_t elapsed_ms) {
  std::lock_guard<std::mutex> timing_lock(timing_mutex_);
  timing_.send_frame_ms += elapsed_ms;
  timing_.max_send_frame_ms = std::max(timing_.max_send_frame_ms, static_cast<int>(elapsed_ms));
  ++timing_.frame_count;
}

void MuxSendScheduler::set_error(std::exception_ptr error) {
  bool should_close = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!error_) {
      error_ = error;
      if (!closing_) {
        closing_ = true;
        should_close = true;
      }
    }
  }
  if (should_close) close_channels();
  cv_.notify_all();
}

void MuxSendScheduler::close_channels() {
  for (auto& channel : channels_) channel.close();
}

void MuxSendScheduler::worker(std::size_t channel_index) {
  while (true) {
    SendItem item;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [&] { return error_ || finishing_ || !queues_[channel_index].empty(); });
      if (error_) return;
      if (queues_[channel_index].empty()) {
        if (finishing_) break;
        continue;
      }
      item = std::move(queues_[channel_index].front());
      queues_[channel_index].pop_front();
    }

    std::int64_t elapsed_ms = 0;
    try {
      const auto send_start = TransferClock::now();
      send_tagged(channels_[channel_index], ciphers_[channel_index], StreamTag::Data, item.payload);
      elapsed_ms = transfer_elapsed_ms_since(send_start);
      record_send_timing(elapsed_ms);
      {
        std::lock_guard<std::mutex> lock(report_mutex_);
        reporter_.file_advance(item.raw_size);
      }
    } catch (...) {
      set_error(std::current_exception());
      return;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto& state = channel_state_[channel_index];
      state.pending_bytes -= std::min<std::uint64_t>(state.pending_bytes, item.payload.size());
      record_channel_latency_locked(channel_index, elapsed_ms);
    }
    cv_.notify_all();
  }

  try {
    const auto send_start = TransferClock::now();
    send_tagged(channels_[channel_index], ciphers_[channel_index], StreamTag::ChunkEnd, std::span<const std::uint8_t>());
    record_send_timing(transfer_elapsed_ms_since(send_start));
  } catch (...) {
    set_error(std::current_exception());
  }
}

}  // namespace kiko::detail
