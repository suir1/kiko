#include "transfer_heuristics.hpp"

#include "core/common.hpp"

namespace kiko {
namespace {

std::string lower_ext(const std::filesystem::path& path) {
  auto ext = path.extension().string();
  if (!ext.empty() && ext[0] == '.') ext.erase(ext.begin());
  return lowercase_ascii(ext);
}

bool is_precompressed_ext(const std::string& ext) {
  static constexpr const char* kExts[] = {
      "7z",   "avi",  "br",   "bz2",  "flac", "gif",  "gz",   "jpeg", "jpg",  "lz4",
      "mkv",  "mov",  "mp3",  "mp4",  "ogg",  "opus", "png",  "rar",  "webp", "xz",
      "zip",  "zst",  "tgz",  "tbz2", "txz",  "wasm", "pdf",  "heic", "heif", "avif",
      "jxl",
  };
  for (const char* candidate : kExts) {
    if (ext == candidate) return true;
  }
  return false;
}

}  // namespace

bool should_compress_path(const std::filesystem::path& path) {
  return !is_precompressed_ext(lower_ext(path));
}

int recommend_connections(std::int64_t relay_rtt_ms, std::uint64_t total_bytes) {
  if (total_bytes < 4 * 1024 * 1024) return 1;
  if (relay_rtt_ms < 0) return 4;
  if (relay_rtt_ms > 150) return total_bytes > 100 * 1024 * 1024 ? 4 : 2;
  if (total_bytes > 500 * 1024 * 1024) return 8;
  if (total_bytes > 50 * 1024 * 1024) return 6;
  return 4;
}

}  // namespace kiko
