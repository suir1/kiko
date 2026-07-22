#pragma once

#include <filesystem>
#include <string>

namespace kiko {

enum class NativePickMode {
  File,
  Directory,
};

enum class NativePickStatus {
  Selected,
  Canceled,
  Unavailable,
  Error,
};

struct NativePickResult {
  NativePickStatus status = NativePickStatus::Unavailable;
  std::filesystem::path path;
  std::string error;
};

[[nodiscard]] NativePickResult pick_native_path(NativePickMode mode);

} // namespace kiko
