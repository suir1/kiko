#include "web/web_upload.hpp"

#include <array>
#include <fstream>
#include <map>
#include <mutex>
#include <random>

namespace kiko {
namespace {

constexpr std::size_t kMaxUploadChunkBytes = 768 * 1024;

std::string random_id() {
  static constexpr char hex[] = "0123456789abcdef";
  std::random_device random;
  std::uniform_int_distribution<int> distribution(0, 255);
  std::string out(32, '0');
  for (std::size_t i = 0; i < out.size(); i += 2) {
    const auto value = static_cast<unsigned int>(distribution(random));
    out[i] = hex[(value >> 4) & 0x0f];
    out[i + 1] = hex[value & 0x0f];
  }
  return out;
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
  };

  mutable std::mutex mutex;
  std::filesystem::path root;
  std::map<std::string, Upload> uploads;
};

WebUploadStore::WebUploadStore() : impl_(std::make_unique<Impl>()) {
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
  std::error_code ec;
  const auto space = std::filesystem::space(impl_->root, ec);
  if (!ec && size > space.available) {
    error = "not enough free space to stage selected file";
    return std::nullopt;
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
  impl_->uploads.emplace(id, Impl::Upload{path, size, 0, false});
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

  std::ofstream file(upload.path, std::ios::binary | std::ios::app);
  if (!file) {
    error = "could not open staged upload file";
    return false;
  }
  file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!file) {
    error = "could not write staged upload file";
    return false;
  }
  upload.received += static_cast<std::uint64_t>(bytes.size());
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
  return CompletedWebUpload{id, upload.path, upload.expected};
}

std::optional<std::filesystem::path>
WebUploadStore::completed_path(const std::string &id,
                               std::string &error) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto it = impl_->uploads.find(id);
  if (it == impl_->uploads.end() || !it->second.finished) {
    error = "completed upload not found";
    return std::nullopt;
  }
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
  std::error_code ec;
  std::filesystem::remove(it->second.path, ec);
  ec.clear();
  std::filesystem::remove(it->second.path.parent_path(), ec);
  impl_->uploads.erase(it);
}

} // namespace kiko
