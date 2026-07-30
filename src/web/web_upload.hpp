#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace kiko {

struct CompletedWebUpload {
  std::string id;
  std::filesystem::path path;
  std::uint64_t size = 0;
};

struct WebUploadLimits {
  std::size_t max_active_uploads = 8;
  std::uint64_t free_space_reserve_bytes = 64ull * 1024ull * 1024ull;
  std::chrono::seconds idle_ttl{std::chrono::hours(1)};
};

class WebUploadStore {
public:
  explicit WebUploadStore(WebUploadLimits limits = {});
  WebUploadStore(const WebUploadStore &) = delete;
  WebUploadStore &operator=(const WebUploadStore &) = delete;
  ~WebUploadStore();

  [[nodiscard]] std::optional<std::string>
  start(std::string filename, std::uint64_t size, std::string &error);
  [[nodiscard]] bool append(const std::string &id, std::uint64_t offset,
                            std::string_view bytes, std::string &error);
  [[nodiscard]] std::optional<CompletedWebUpload> finish(const std::string &id,
                                                         std::string &error);
  [[nodiscard]] std::optional<std::filesystem::path>
  completed_path(const std::string &id, std::string &error);
  void release(const std::string &id);
  void cancel(const std::string &id);
  void purge_expired(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace kiko
