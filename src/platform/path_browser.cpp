#include "platform/path_browser.hpp"

#include "core/common.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <utility>

namespace kiko {
namespace {

constexpr const char* kParentLabel = "../";
constexpr const char* kSelectHereLabel = "[Select this folder]";

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

void load_modified_time(PathBrowserEntry& entry) {
  std::error_code ec;
  entry.modified = std::filesystem::last_write_time(entry.path, ec);
  entry.has_modified = !ec;
}

void sort_entries(std::vector<PathBrowserEntry>& entries, PathBrowserSort sort) {
  const auto by_name = [](const PathBrowserEntry& a, const PathBrowserEntry& b) {
    return a.label < b.label;
  };
  if (sort == PathBrowserSort::Name) {
    std::sort(entries.begin(), entries.end(), by_name);
    return;
  }

  std::sort(entries.begin(), entries.end(), [&](const PathBrowserEntry& a, const PathBrowserEntry& b) {
    if (a.has_modified != b.has_modified) return a.has_modified;
    if (a.has_modified && b.has_modified && a.modified != b.modified) return a.modified > b.modified;
    return by_name(a, b);
  });
}

}  // namespace

std::filesystem::path normalize_browser_directory(const std::filesystem::path& path) {
  std::error_code ec;
  auto absolute = std::filesystem::absolute(path, ec);
  if (ec) return path;
  if (std::filesystem::is_directory(absolute, ec) && !ec) return absolute;
  return absolute.parent_path();
}

std::vector<PathBrowserEntry> list_browser_directory(const std::filesystem::path& dir, PathPickMode mode,
                                                     PathBrowserSort sort) {
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec) || ec) {
    throw KikoError("not a directory: " + dir.string());
  }

  std::vector<PathBrowserEntry> out;
  if (dir.has_parent_path()) {
    PathBrowserEntry parent;
    parent.label = kParentLabel;
    parent.path = dir.parent_path();
    parent.is_dir = true;
    parent.parent = true;
    out.push_back(std::move(parent));
  }

  std::vector<PathBrowserEntry> dirs;
  std::vector<PathBrowserEntry> files;
  std::filesystem::directory_iterator iterator(dir, ec);
  const std::filesystem::directory_iterator end;
  while (!ec && iterator != end) {
    const auto& item = *iterator;
    PathBrowserEntry entry;
    entry.path = item.path();
    std::error_code type_ec;
    entry.is_dir = item.is_directory(type_ec);
    if (!type_ec) {
      const auto name = item.path().filename().string();
      if (!name.empty() && name != ".") {
        entry.label = entry.is_dir ? name + "/" : name;
        entry.selectable = entry.is_dir || mode == PathPickMode::FileOrDirectory;
        load_modified_time(entry);
        if (entry.is_dir) {
          dirs.push_back(std::move(entry));
        } else if (mode == PathPickMode::FileOrDirectory) {
          files.push_back(std::move(entry));
        }
      }
    }
    iterator.increment(ec);
  }

  sort_entries(dirs, sort);
  sort_entries(files, sort);
  if (mode == PathPickMode::FileOrDirectory) {
    out.insert(out.end(), files.begin(), files.end());
    out.insert(out.end(), dirs.begin(), dirs.end());
  } else {
    out.insert(out.end(), dirs.begin(), dirs.end());
  }

  PathBrowserEntry here;
  here.label = kSelectHereLabel;
  here.path = dir;
  here.is_dir = true;
  here.selectable = true;
  here.select_here = true;
  out.push_back(std::move(here));
  return out;
}

std::vector<PathBrowserEntry> filter_browser_entries(const std::vector<PathBrowserEntry>& entries,
                                                     const std::string& filter) {
  if (filter.empty()) return entries;

  const auto lowered_filter = lower(filter);
  std::vector<PathBrowserEntry> out;
  out.reserve(entries.size());
  for (const auto& entry : entries) {
    if (entry.parent || entry.select_here ||
        lower(entry.path.filename().string()).find(lowered_filter) != std::string::npos) {
      out.push_back(entry);
    }
  }
  return out;
}

std::vector<PathBrowserEntry> browse_directory(const std::filesystem::path& dir, PathPickMode mode,
                                               PathBrowserSort sort, const std::string& filter) {
  return filter_browser_entries(list_browser_directory(dir, mode, sort), filter);
}

}  // namespace kiko
