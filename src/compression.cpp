#include "compression.hpp"

#include <zstd.h>

#include <algorithm>

namespace kiko {

ZstdStreamCompressor::ZstdStreamCompressor(int level) {
  auto* stream = ZSTD_createCStream();
  if (!stream) throw KikoError("failed to create zstd compressor");
  stream_ = stream;
  auto rc = ZSTD_initCStream(stream, level);
  if (ZSTD_isError(rc)) throw KikoError(std::string("zstd init compressor failed: ") + ZSTD_getErrorName(rc));
}

ZstdStreamCompressor::~ZstdStreamCompressor() {
  if (stream_) ZSTD_freeCStream(static_cast<ZSTD_CStream*>(stream_));
}

Bytes ZstdStreamCompressor::compress(std::span<const std::uint8_t> input, bool finish) {
  ZSTD_inBuffer in{input.data(), input.size(), 0};
  Bytes out;
  std::size_t chunk_size = ZSTD_CStreamOutSize();
  Bytes chunk(chunk_size);

  while (in.pos < in.size || finish) {
    ZSTD_outBuffer zstd_out{chunk.data(), chunk.size(), 0};
    std::size_t rc = finish ? ZSTD_endStream(static_cast<ZSTD_CStream*>(stream_), &zstd_out)
                            : ZSTD_compressStream(static_cast<ZSTD_CStream*>(stream_), &zstd_out, &in);
    if (ZSTD_isError(rc)) throw KikoError(std::string("zstd compress failed: ") + ZSTD_getErrorName(rc));
    out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(zstd_out.pos));
    if (!finish && in.pos == in.size) break;
    if (finish && rc == 0) break;
    if (input.empty() && zstd_out.pos == 0 && rc == 0) break;
  }
  return out;
}

ZstdStreamDecompressor::ZstdStreamDecompressor() {
  auto* stream = ZSTD_createDStream();
  if (!stream) throw KikoError("failed to create zstd decompressor");
  stream_ = stream;
  auto rc = ZSTD_initDStream(stream);
  if (ZSTD_isError(rc)) throw KikoError(std::string("zstd init decompressor failed: ") + ZSTD_getErrorName(rc));
}

ZstdStreamDecompressor::~ZstdStreamDecompressor() {
  if (stream_) ZSTD_freeDStream(static_cast<ZSTD_DStream*>(stream_));
}

Bytes ZstdStreamDecompressor::decompress(std::span<const std::uint8_t> input, std::size_t max_output) {
  ZSTD_inBuffer in{input.data(), input.size(), 0};
  Bytes out;
  std::size_t chunk_size = ZSTD_DStreamOutSize();
  Bytes chunk(chunk_size);

  while (in.pos < in.size) {
    ZSTD_outBuffer zstd_out{chunk.data(), chunk.size(), 0};
    auto rc = ZSTD_decompressStream(static_cast<ZSTD_DStream*>(stream_), &zstd_out, &in);
    if (ZSTD_isError(rc)) throw KikoError(std::string("zstd decompress failed: ") + ZSTD_getErrorName(rc));
    if (zstd_out.pos > max_output - out.size()) throw KikoError("zstd decompressed data exceeds declared size");
    out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(zstd_out.pos));
  }
  return out;
}

Bytes zstd_compress_block(std::span<const std::uint8_t> input, int level) {
  Bytes out(ZSTD_compressBound(input.size()));
  std::size_t rc = ZSTD_compress(out.data(), out.size(), input.data(), input.size(), level);
  if (ZSTD_isError(rc)) throw KikoError(std::string("zstd block compress failed: ") + ZSTD_getErrorName(rc));
  out.resize(rc);
  return out;
}

Bytes zstd_decompress_block(std::span<const std::uint8_t> input, std::size_t expected_size) {
  Bytes out(expected_size);
  std::size_t rc = ZSTD_decompress(out.data(), out.size(), input.data(), input.size());
  if (ZSTD_isError(rc)) throw KikoError(std::string("zstd block decompress failed: ") + ZSTD_getErrorName(rc));
  if (rc != expected_size) throw KikoError("zstd block decompress size mismatch");
  out.resize(rc);
  return out;
}

}  // namespace kiko
