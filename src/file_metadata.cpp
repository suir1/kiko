#include "file_metadata.hpp"

#include <chrono>

namespace kiko::detail {
namespace {

std::chrono::system_clock::time_point file_time_to_system(std::filesystem::file_time_type file_time) {
  return std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      file_time - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
}

std::filesystem::file_time_type system_time_to_file(std::chrono::system_clock::time_point system_time) {
  return std::chrono::time_point_cast<std::filesystem::file_time_type::duration>(
      system_time - std::chrono::system_clock::now() + std::filesystem::file_time_type::clock::now());
}

}  // namespace

std::uint64_t file_mtime_ms(const std::filesystem::path& path) {
  std::error_code ec;
  auto ft = std::filesystem::last_write_time(path, ec);
  if (ec) return 0;
  auto sys = file_time_to_system(ft);
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(sys.time_since_epoch()).count());
}

void apply_mtime_ms(const std::filesystem::path& path, std::uint64_t mtime_ms) {
  if (mtime_ms == 0) return;
  std::error_code ec;
  auto sys = std::chrono::system_clock::time_point{std::chrono::milliseconds{mtime_ms}};
  auto ft = system_time_to_file(sys);
  std::filesystem::last_write_time(path, ft, ec);
}

void apply_file_mtime(const std::filesystem::path& path, const Message& header) {
  apply_mtime_ms(path, header.get_u64("mtime_ms", 0));
}

}  // namespace kiko::detail
