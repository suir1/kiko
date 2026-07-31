#include "platform/atomic_file.hpp"

#include "core/common.hpp"
#include "core/random.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace kiko {
namespace {

struct TempFileCleanup {
  std::filesystem::path path;

  ~TempFileCleanup() {
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
};

std::optional<std::string> replace_file_impl(const std::filesystem::path& source,
                                             const std::filesystem::path& destination) {
#ifdef _WIN32
  if (MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return std::nullopt;
  }
  const std::error_code ec(static_cast<int>(GetLastError()), std::system_category());
  return "failed to replace " + destination.string() + ": " + ec.message();
#else
  std::error_code ec;
  std::filesystem::rename(source, destination, ec);
  if (!ec) return std::nullopt;
  return "failed to replace " + destination.string() + ": " + ec.message();
#endif
}

std::optional<std::string> write_private_temp_file(const std::filesystem::path& path, std::string_view contents) {
#ifdef _WIN32
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) return "failed to open temporary file " + path.string();
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  output.flush();
  if (!output) return "failed to write temporary file " + path.string();
  output.close();
  if (output.fail()) return "failed to close temporary file " + path.string();
  return std::nullopt;
#else
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
  if (fd < 0) return "failed to open temporary file " + path.string() + ": " + std::strerror(errno);

  const char* data = contents.data();
  std::size_t remaining = contents.size();
  while (remaining > 0) {
    const auto chunk = std::min(remaining, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
    const auto written = ::write(fd, data, chunk);
    if (written < 0) {
      if (errno == EINTR) continue;
      const auto error = std::string("failed to write temporary file ") + path.string() + ": " +
                         std::strerror(errno);
      (void)::close(fd);
      return error;
    }
    if (written == 0) {
      (void)::close(fd);
      return "failed to write temporary file " + path.string() + ": zero-byte write";
    }
    data += written;
    remaining -= static_cast<std::size_t>(written);
  }
  if (::fsync(fd) != 0) {
    const auto error = std::string("failed to sync temporary file ") + path.string() + ": " + std::strerror(errno);
    (void)::close(fd);
    return error;
  }
  if (::close(fd) != 0) {
    return "failed to close temporary file " + path.string() + ": " + std::strerror(errno);
  }
  return std::nullopt;
#endif
}

}  // namespace

std::optional<std::string> atomic_replace_file(const std::filesystem::path& source,
                                               const std::filesystem::path& destination) {
  if (source.empty() || destination.empty()) return "replacement path is empty";
  return replace_file_impl(source, destination);
}

std::optional<std::string> atomic_write_text_file(const std::filesystem::path& path, std::string_view contents) {
  if (path.empty() || path.filename().empty()) return "persistence path is empty";

  std::error_code ec;
  const auto parent = path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, ec);
    if (ec) return "failed to create " + parent.string() + ": " + ec.message();
  }

  auto temporary = path;
  temporary += ".tmp-" + random_code(8);
  TempFileCleanup cleanup{temporary};
  if (auto error = write_private_temp_file(temporary, contents)) return error;

  if (auto error = atomic_replace_file(temporary, path)) return error;
  cleanup.path.clear();
  return std::nullopt;
}

std::optional<std::string> move_file_to_recovery_backup(const std::filesystem::path& path, std::string_view tag,
                                                        std::filesystem::path& backup_path) {
  if (tag.empty() || tag.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-_") != std::string_view::npos) {
    return "invalid recovery backup tag";
  }
  backup_path = path;
  backup_path += "." + std::string(tag) + "-" + std::to_string(now_ms()) + "-" + random_code(3);
  std::error_code ec;
  std::filesystem::rename(path, backup_path, ec);
  if (!ec) return std::nullopt;
  backup_path.clear();
  return "failed to preserve " + path.string() + ": " + ec.message();
}

}  // namespace kiko
