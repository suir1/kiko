#pragma once

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

class WebUploadStore {
public:
  WebUploadStore();
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
  completed_path(const std::string &id, std::string &error) const;
  void release(const std::string &id);
  void cancel(const std::string &id);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace kiko
