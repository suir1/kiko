#include "transfer.hpp"

#include "file_metadata.hpp"
#include "gitignore.hpp"
#include "imohash.hpp"

namespace kiko {

std::vector<FileEntry> collect_files(const std::filesystem::path& path, const CollectOptions& options) {
  std::vector<FileEntry> entries;
  std::error_code ec;
  if (std::filesystem::is_directory(path, ec)) {
    auto base = path.parent_path();
    if (base.empty()) base = ".";
    GitIgnore gitignore;
    if (options.use_gitignore) gitignore = load_gitignore_stack(path);
    for (auto it = std::filesystem::recursive_directory_iterator(path);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
      if (it->is_regular_file()) {
        auto rel = std::filesystem::relative(it->path(), base).generic_string();
        if (options.use_gitignore && gitignore.ignored(rel)) continue;
        FileEntry entry{it->path(), rel, std::filesystem::file_size(it->path()), ""};
        entry.imohash = imohash_hex(entry.absolute);
        entry.mtime_ms = detail::file_mtime_ms(entry.absolute);
        entry.mode = detail::file_mode_bits(entry.absolute);
        entries.push_back(std::move(entry));
      } else if (it->is_directory()) {
        std::filesystem::directory_iterator dir_it(it->path(), ec);
        if (ec || dir_it != std::filesystem::directory_iterator()) continue;
        auto rel = std::filesystem::relative(it->path(), base).generic_string() + "/";
        if (options.use_gitignore && gitignore.ignored(rel.substr(0, rel.size() - 1))) continue;
        FileEntry entry{it->path(), rel, 0, ""};
        entry.mtime_ms = detail::file_mtime_ms(entry.absolute);
        entry.mode = detail::file_mode_bits(entry.absolute);
        entries.push_back(std::move(entry));
      }
    }
    if (entries.empty()) {
      auto rel = path.filename().string() + "/";
      FileEntry entry{path, rel, 0, ""};
      entry.mtime_ms = detail::file_mtime_ms(path);
      entry.mode = detail::file_mode_bits(path);
      entries.push_back(std::move(entry));
    }
  } else if (std::filesystem::is_regular_file(path, ec)) {
    FileEntry entry{path, path.filename().string(), std::filesystem::file_size(path), ""};
    entry.imohash = imohash_hex(entry.absolute);
    entry.mtime_ms = detail::file_mtime_ms(entry.absolute);
    entry.mode = detail::file_mode_bits(entry.absolute);
    entries.push_back(std::move(entry));
  } else {
    throw KikoError("not a file or directory: " + path.string());
  }
  return entries;
}

}  // namespace kiko
