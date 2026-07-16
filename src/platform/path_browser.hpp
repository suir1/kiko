#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace kiko {

enum class PathPickMode {
  FileOrDirectory,
  DirectoryOnly,
};

enum class PathBrowserSort {
  Name,
  ModifiedDesc,
};

struct PathBrowserEntry {
  std::string label;
  std::filesystem::path path;
  bool is_dir = false;
  bool selectable = false;
  bool parent = false;
  bool select_here = false;
  std::filesystem::file_time_type modified{};
  bool has_modified = false;
};

[[nodiscard]] std::filesystem::path normalize_browser_directory(const std::filesystem::path& path);
[[nodiscard]] std::vector<PathBrowserEntry> list_browser_directory(const std::filesystem::path& dir,
                                                                   PathPickMode mode,
                                                                   PathBrowserSort sort);
[[nodiscard]] std::vector<PathBrowserEntry> filter_browser_entries(const std::vector<PathBrowserEntry>& entries,
                                                                   const std::string& filter);

}  // namespace kiko
