#include "file_metadata.hpp"

#include <chrono>

namespace kiko::detail {
namespace {

constexpr std::uint32_t kOwnerExec = 0100;
constexpr std::uint32_t kGroupExec = 0010;
constexpr std::uint32_t kOthersExec = 0001;
constexpr std::uint32_t kExecMask = kOwnerExec | kGroupExec | kOthersExec;

std::chrono::system_clock::time_point file_time_to_system(std::filesystem::file_time_type file_time) {
#ifdef _WIN32
  const auto file_now = std::filesystem::file_time_type::clock::now();
  const auto system_now = std::chrono::system_clock::now();
  return std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      file_time - file_now + system_now);
#else
  return std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      std::chrono::file_clock::to_sys(file_time));
#endif
}

std::filesystem::file_time_type system_time_to_file(std::chrono::system_clock::time_point system_time) {
#ifdef _WIN32
  const auto file_now = std::filesystem::file_time_type::clock::now();
  const auto system_now = std::chrono::system_clock::now();
  return std::chrono::time_point_cast<std::filesystem::file_time_type::duration>(
      system_time - system_now + file_now);
#else
  return std::chrono::file_clock::from_sys(system_time);
#endif
}

std::filesystem::perms mode_to_exec_perms(std::uint32_t mode) {
  auto perms = std::filesystem::perms::none;
  if ((mode & kOwnerExec) != 0) perms |= std::filesystem::perms::owner_exec;
  if ((mode & kGroupExec) != 0) perms |= std::filesystem::perms::group_exec;
  if ((mode & kOthersExec) != 0) perms |= std::filesystem::perms::others_exec;
  return perms;
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

std::uint32_t file_mode_bits(const std::filesystem::path& path) {
  std::error_code ec;
  const auto perms = std::filesystem::status(path, ec).permissions();
  if (ec) return 0;
  std::uint32_t mode = 0;
  if ((perms & std::filesystem::perms::owner_exec) != std::filesystem::perms::none) mode |= kOwnerExec;
  if ((perms & std::filesystem::perms::group_exec) != std::filesystem::perms::none) mode |= kGroupExec;
  if ((perms & std::filesystem::perms::others_exec) != std::filesystem::perms::none) mode |= kOthersExec;
  return mode;
}

void apply_mtime_ms(const std::filesystem::path& path, std::uint64_t mtime_ms) {
  if (mtime_ms == 0) return;
  std::error_code ec;
  auto sys = std::chrono::system_clock::time_point{std::chrono::milliseconds{mtime_ms}};
  auto ft = system_time_to_file(sys);
  std::filesystem::last_write_time(path, ft, ec);
}

void apply_file_mode_bits(const std::filesystem::path& path, std::uint32_t mode) {
  const auto exec_mode = mode & kExecMask;
  if (exec_mode == 0) return;
  std::error_code ec;
  std::filesystem::permissions(path, mode_to_exec_perms(exec_mode), std::filesystem::perm_options::add, ec);
}

void apply_file_metadata(const std::filesystem::path& path, const Message& header) {
  apply_file_mode_bits(path, static_cast<std::uint32_t>(header.get_u64("mode", 0)));
  apply_mtime_ms(path, header.get_u64("mtime_ms", 0));
}

}  // namespace kiko::detail
