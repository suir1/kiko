#include "web/web_upload.hpp"

#include "core/random.hpp"

#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <utility>

namespace kiko {
namespace {

constexpr std::size_t kMaxUploadChunkBytes = 768 * 1024;

std::string random_id() {
  return secure_random_hex(16);
}

std::optional<std::filesystem::path>
safe_filename(const std::string &filename) {
  if (filename.empty() || filename.find('\0') != std::string::npos)
    return std::nullopt;
  auto safe = std::filesystem::path(filename).filename();
  if (safe.empty() || safe == "." || safe == "..")
    return std::nullopt;
  return safe;
}

} // namespace

struct WebUploadStore::Impl {
  struct Upload {
    std::filesystem::path path;
    std::uint64_t expected = 0;
    std::uint64_t received = 0;
    bool finished = false;
    std::chrono::steady_clock::time_point updated_at = std::chrono::steady_clock::now();
  };

  mutable std::mutex mutex;
  std::filesystem::path root;
  std::map<std::string, Upload> uploads;
  WebUploadLimits limits;

  explicit Impl(WebUploadLimits value) : limits(std::move(value)) {}

  void erase_upload(std::map<std::string, Upload>::iterator it) {
    std::error_code ec;
    std::filesystem::remove(it->second.path, ec);
    ec.clear();
    std::filesystem::remove(it->second.path.parent_path(), ec);
    uploads.erase(it);
  }

  void purge_expired(std::chrono::steady_clock::time_point now) {
    for (auto it = uploads.begin(); it != uploads.end();) {
      if (limits.idle_ttl.count() > 0 && now - it->second.updated_at <= limits.idle_ttl) {
        ++it;
        continue;
      }
      auto expired = it++;
      erase_upload(expired);
    }
  }

  std::uint64_t reserved_remaining_bytes() const {
    std::uint64_t total = 0;
    for (const auto &[_, upload] : uploads) {
      const auto remaining = upload.expected - upload.received;
      if (remaining > std::numeric_limits<std::uint64_t>::max() - total) {
        return std::numeric_limits<std::uint64_t>::max();
      }
      total += remaining;
    }
    return total;
  }
};

WebUploadStore::WebUploadStore(WebUploadLimits limits)
    : impl_(std::make_unique<Impl>(std::move(limits))) {
  impl_->root = std::filesystem::temp_directory_path() /
                ("kiko-web-uploads-" + random_id());
  std::filesystem::create_directories(impl_->root);
}

WebUploadStore::~WebUploadStore() {
  std::error_code ec;
  std::filesystem::remove_all(impl_->root, ec);
}

std::optional<std::string> WebUploadStore::start(std::string filename,
                                                 std::uint64_t size,
                                                 std::string &error) {
  const auto safe = safe_filename(filename);
  if (!safe) {
    error = "upload filename is invalid";
    return std::nullopt;
  }

  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->purge_expired(std::chrono::steady_clock::now());
  if (impl_->uploads.size() >= impl_->limits.max_active_uploads) {
    error = "too many active browser uploads";
    return std::nullopt;
  }
  std::error_code ec;
  const auto space = std::filesystem::space(impl_->root, ec);
  if (ec) {
    error = "could not inspect free space for browser upload";
    return std::nullopt;
  }
  if (size > 0) {
    const auto reserve = static_cast<std::uintmax_t>(impl_->limits.free_space_reserve_bytes);
    const auto usable = space.available > reserve ? space.available - reserve : 0;
    const auto reserved = static_cast<std::uintmax_t>(impl_->reserved_remaining_bytes());
    if (static_cast<std::uintmax_t>(size) > usable || reserved > usable - static_cast<std::uintmax_t>(size)) {
      error = "not enough unreserved space to stage selected file";
      return std::nullopt;
    }
  }
  std::string id;
  do {
    id = random_id();
  } while (impl_->uploads.contains(id));

  const auto directory = impl_->root / id;
  const auto path = directory / *safe;
  ec.clear();
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    error = "could not create upload directory: " + ec.message();
    return std::nullopt;
  }
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    error = "could not create staged upload file";
    std::filesystem::remove(directory, ec);
    return std::nullopt;
  }
  file.close();
  impl_->uploads.emplace(id, Impl::Upload{path, size, 0, false, std::chrono::steady_clock::now()});
  return id;
}

bool WebUploadStore::append(const std::string &id, std::uint64_t offset,
                            std::string_view bytes, std::string &error) {
  if (bytes.size() > kMaxUploadChunkBytes) {
    error = "upload chunk exceeds 768 KiB limit";
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto it = impl_->uploads.find(id);
  if (it == impl_->uploads.end()) {
    error = "upload not found";
    return false;
  }
  auto &upload = it->second;
  if (upload.finished) {
    error = "upload is already complete";
    return false;
  }
  if (offset != upload.received) {
    error = "upload chunk offset mismatch";
    return false;
  }
  if (bytes.size() > upload.expected - upload.received) {
    error = "upload exceeds declared size";
    return false;
  }

  std::error_code size_error;
  const auto staged_size = std::filesystem::file_size(upload.path, size_error);
  if (size_error || staged_size != upload.received) {
    error = "staged upload size no longer matches confirmed offset";
    return false;
  }

  std::ofstream file(upload.path, std::ios::binary | std::ios::app);
  if (!file) {
    error = "could not open staged upload file";
    return false;
  }
  file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  file.flush();
  if (!file) {
    file.close();
    std::error_code rollback_error;
    std::filesystem::resize_file(upload.path, upload.received, rollback_error);
    error = "could not write staged upload file";
    if (rollback_error) error += "; partial upload could not be rolled back";
    return false;
  }
  upload.received += static_cast<std::uint64_t>(bytes.size());
  upload.updated_at = std::chrono::steady_clock::now();
  return true;
}

std::optional<CompletedWebUpload> WebUploadStore::finish(const std::string &id,
                                                         std::string &error) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto it = impl_->uploads.find(id);
  if (it == impl_->uploads.end()) {
    error = "upload not found";
    return std::nullopt;
  }
  auto &upload = it->second;
  if (upload.received != upload.expected) {
    error = "upload is incomplete";
    return std::nullopt;
  }
  upload.finished = true;
  upload.updated_at = std::chrono::steady_clock::now();
  return CompletedWebUpload{id, upload.path, upload.expected};
}

std::optional<std::filesystem::path>
WebUploadStore::completed_path(const std::string &id,
                               std::string &error) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->purge_expired(std::chrono::steady_clock::now());
  const auto it = impl_->uploads.find(id);
  if (it == impl_->uploads.end() || !it->second.finished) {
    error = "completed upload not found";
    return std::nullopt;
  }
  it->second.updated_at = std::chrono::steady_clock::now();
  return it->second.path;
}

void WebUploadStore::release(const std::string &id) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->uploads.erase(id);
}

void WebUploadStore::cancel(const std::string &id) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto it = impl_->uploads.find(id);
  if (it == impl_->uploads.end())
    return;
  impl_->erase_upload(it);
}

void WebUploadStore::purge_expired(std::chrono::steady_clock::time_point now) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->purge_expired(now);
}

} // namespace kiko
