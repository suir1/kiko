#pragma once

#include "common.hpp"

#include <functional>
#include <limits>
#include <span>

namespace kiko {

class ZstdStreamCompressor {
 public:
  explicit ZstdStreamCompressor(int level = 3);
  ZstdStreamCompressor(const ZstdStreamCompressor&) = delete;
  ZstdStreamCompressor& operator=(const ZstdStreamCompressor&) = delete;
  ~ZstdStreamCompressor();

  [[nodiscard]] Bytes compress(std::span<const std::uint8_t> input, bool finish);

 private:
  void* stream_ = nullptr;
};

class ZstdStreamDecompressor {
 public:
  ZstdStreamDecompressor();
  ZstdStreamDecompressor(const ZstdStreamDecompressor&) = delete;
  ZstdStreamDecompressor& operator=(const ZstdStreamDecompressor&) = delete;
  ~ZstdStreamDecompressor();

  [[nodiscard]] Bytes decompress(std::span<const std::uint8_t> input,
                                 std::size_t max_output = std::numeric_limits<std::size_t>::max());

 private:
  void* stream_ = nullptr;
};

// One-shot block (de)compression for independent, out-of-order chunks used by
// the multiplexed transfer path. `expected_size` is the known decompressed size.
[[nodiscard]] Bytes zstd_compress_block(std::span<const std::uint8_t> input, int level = 3);
[[nodiscard]] Bytes zstd_decompress_block(std::span<const std::uint8_t> input, std::size_t expected_size);

}  // namespace kiko
