#include "transfer_heuristics.hpp"

#include <algorithm>
#include <cctype>

namespace kiko {
namespace {

std::string lower_ext(const std::filesystem::path& path) {
  auto ext = path.extension().string();
  if (!ext.empty() && ext[0] == '.') ext.erase(ext.begin());
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext;
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

TransferPayloadStats transfer_payload_stats(const std::vector<FileEntry>& files) {
  TransferPayloadStats stats;
  std::size_t regular_files = 0;
  std::size_t compressible = 0;
  for (const auto& entry : files) {
    const bool dir_marker = entry.size == 0 && !entry.relative.empty() && entry.relative.back() == '/';
    if (dir_marker || entry.symlink) continue;
    ++regular_files;
    if (entry.size > stats.largest_file_bytes) stats.largest_file_bytes = entry.size;
    if (should_compress_path(entry.absolute)) ++compressible;
  }
  if (regular_files > 0) {
    stats.compressible_ratio = static_cast<double>(compressible) / static_cast<double>(regular_files);
  }
  return stats;
}

}  // namespace kiko
