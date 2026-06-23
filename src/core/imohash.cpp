#include "core/imohash.hpp"

#include "core/common.hpp"

#include <array>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace kiko {
namespace {

constexpr std::size_t kChunk = 128 * 1024;
constexpr std::size_t kSampleWindow = 16 * 16 * 8 * 1024;

inline std::uint64_t rotl64(std::uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }

void murmur3_x64_128(const std::uint8_t* data, std::size_t len, std::uint32_t seed, std::uint64_t out[2]) {
  const std::uint64_t c1 = 0x87c37b91114253d5ULL;
  const std::uint64_t c2 = 0x4cf5ad432745937fULL;
  std::uint64_t h1 = seed;
  std::uint64_t h2 = seed;
  const std::size_t nblocks = len / 16;

  for (std::size_t i = 0; i < nblocks; ++i) {
    std::uint64_t k1 = 0;
    std::uint64_t k2 = 0;
    std::memcpy(&k1, data + i * 16, 8);
    std::memcpy(&k2, data + i * 16 + 8, 8);
    k1 *= c1;
    k1 = rotl64(k1, 31);
    k1 *= c2;
    h1 ^= k1;
    h1 = rotl64(h1, 27);
    h1 += h2;
    h1 = h1 * 5 + 0x52dce729;
    k2 *= c2;
    k2 = rotl64(k2, 33);
    k2 *= c1;
    h2 ^= k2;
    h2 = rotl64(h2, 31);
    h2 += h1;
    h2 = h2 * 5 + 0x38495ab5;
  }

  const std::uint8_t* tail = data + nblocks * 16;
  const std::size_t tail_len = len & 15;
  std::uint64_t k1 = 0;
  std::uint64_t k2 = 0;
  switch (tail_len) {
    case 15:
      k2 ^= static_cast<std::uint64_t>(tail[14]) << 48;
      [[fallthrough]];
    case 14:
      k2 ^= static_cast<std::uint64_t>(tail[13]) << 40;
      [[fallthrough]];
    case 13:
      k2 ^= static_cast<std::uint64_t>(tail[12]) << 32;
      [[fallthrough]];
    case 12:
      k2 ^= static_cast<std::uint64_t>(tail[11]) << 24;
      [[fallthrough]];
    case 11:
      k2 ^= static_cast<std::uint64_t>(tail[10]) << 16;
      [[fallthrough]];
    case 10:
      k2 ^= static_cast<std::uint64_t>(tail[9]) << 8;
      [[fallthrough]];
    case 9:
      k2 ^= static_cast<std::uint64_t>(tail[8]);
      k2 *= c2;
      k2 = rotl64(k2, 33);
      k2 *= c1;
      h2 ^= k2;
      [[fallthrough]];
    case 8:
      k1 ^= static_cast<std::uint64_t>(tail[7]) << 56;
      [[fallthrough]];
    case 7:
      k1 ^= static_cast<std::uint64_t>(tail[6]) << 48;
      [[fallthrough]];
    case 6:
      k1 ^= static_cast<std::uint64_t>(tail[5]) << 40;
      [[fallthrough]];
    case 5:
      k1 ^= static_cast<std::uint64_t>(tail[4]) << 32;
      [[fallthrough]];
    case 4:
      k1 ^= static_cast<std::uint64_t>(tail[3]) << 24;
      [[fallthrough]];
    case 3:
      k1 ^= static_cast<std::uint64_t>(tail[2]) << 16;
      [[fallthrough]];
    case 2:
      k1 ^= static_cast<std::uint64_t>(tail[1]) << 8;
      [[fallthrough]];
    case 1:
      k1 ^= static_cast<std::uint64_t>(tail[0]);
      k1 *= c1;
      k1 = rotl64(k1, 31);
      k1 *= c2;
      h1 ^= k1;
  }

  h1 ^= static_cast<std::uint64_t>(len);
  h2 ^= static_cast<std::uint64_t>(len);
  h1 += h2;
  h2 += h1;
  h1 ^= h1 >> 33;
  h1 *= 0xff51afd7ed558ccdULL;
  h1 ^= h1 >> 33;
  h1 *= 0xc4ceb9fe1a85ec53ULL;
  h1 ^= h1 >> 33;
  h2 ^= h2 >> 33;
  h2 *= 0xff51afd7ed558ccdULL;
  h2 ^= h2 >> 33;
  h2 *= 0xc4ceb9fe1a85ec53ULL;
  h2 ^= h2 >> 33;
  h1 += h2;
  h2 += h1;
  out[0] = h1;
  out[1] = h2;
}

bool read_at(std::ifstream& in, std::uint64_t offset, std::vector<std::uint8_t>& buf) {
  in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
  return in.gcount() == static_cast<std::streamsize>(buf.size());
}

}  // namespace

std::string imohash_hex(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw KikoError("imohash: failed to open " + path.string());
  in.seekg(0, std::ios::end);
  const auto size = static_cast<std::uint64_t>(in.tellg());
  in.seekg(0, std::ios::beg);

  std::vector<std::uint8_t> chunk(kChunk);
  std::vector<std::uint8_t> samples;
  if (size <= kChunk * 2) {
    samples.resize(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(samples.data()), static_cast<std::streamsize>(size));
    if (static_cast<std::uint64_t>(in.gcount()) != size) throw KikoError("imohash: read failed");
  } else {
    if (!read_at(in, 0, chunk)) throw KikoError("imohash: read head failed");
    samples.insert(samples.end(), chunk.begin(), chunk.end());
    if (!read_at(in, size - kChunk, chunk)) throw KikoError("imohash: read tail failed");
    samples.insert(samples.end(), chunk.begin(), chunk.end());

    const auto windows = std::max<std::uint64_t>(1, (size - kChunk) / kSampleWindow);
    for (std::uint64_t w = 0; w < windows && samples.size() < kSampleWindow; ++w) {
      const auto off = w * kSampleWindow + (kSampleWindow / 2);
      if (off + kChunk > size) break;
      if (!read_at(in, off, chunk)) break;
      samples.insert(samples.end(), chunk.begin(), chunk.end());
    }
  }

  std::uint64_t digest[2]{};
  murmur3_x64_128(samples.data(), samples.size(), 0, digest);
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (int i = 0; i < 2; ++i) {
    oss << std::setw(16) << digest[i];
  }
  return oss.str();
}

}  // namespace kiko
