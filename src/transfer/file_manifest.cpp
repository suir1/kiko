#include "transfer.hpp"

#include "file_metadata.hpp"
#include "transfer/gitignore.hpp"
#include "core/imohash.hpp"

namespace kiko {
namespace {

bool target_is_safe_relative_symlink(const std::filesystem::path& target) {
  if (target.empty() || target.is_absolute()) return false;
  for (const auto& part : target) {
    if (part == "..") return false;
  }
  return true;
}

std::string relative_lexical(const std::filesystem::path& path, const std::filesystem::path& base) {
  auto rel = path.lexically_relative(base);
  if (rel.empty()) rel = std::filesystem::relative(path, base);
  return rel.generic_string();
}

FileEntry make_symlink_entry(const std::filesystem::path& path, const std::filesystem::path& base) {
  std::error_code ec;
  const auto target = std::filesystem::read_symlink(path, ec);
  if (ec) throw KikoError("failed to read symlink: " + path.string());
  if (!target_is_safe_relative_symlink(target)) {
    throw KikoError("unsafe symlink target for " + path.string() + ": " + target.generic_string());
  }
  auto rel = relative_lexical(path, base);
  FileEntry entry;
  entry.absolute = path;
  entry.relative = rel;
  entry.symlink = true;
  entry.link_target = target.generic_string();
  return entry;
}

}  // namespace

std::vector<FileEntry> collect_files(const std::filesystem::path& path, const CollectOptions& options) {
  std::vector<FileEntry> entries;
  std::error_code ec;
  const auto path_status = std::filesystem::symlink_status(path, ec);
  const bool preserve_top_level_symlink =
      options.symlink_mode == SymlinkMode::Preserve && !ec && std::filesystem::is_symlink(path_status);
  ec.clear();
  if (preserve_top_level_symlink) {
    auto base = path.parent_path();
    if (base.empty()) base = ".";
    entries.push_back(make_symlink_entry(path, base));
  } else if (std::filesystem::is_directory(path, ec)) {
    auto base = path.parent_path();
    if (base.empty()) base = ".";
    GitIgnore gitignore;
    if (options.use_gitignore) gitignore = load_gitignore_stack(path);
    for (auto it = std::filesystem::recursive_directory_iterator(path);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
      const bool is_symlink = it->is_symlink(ec);
      if (ec) {
        ec.clear();
        continue;
      }
      if (options.symlink_mode == SymlinkMode::Preserve && is_symlink) {
        auto rel = relative_lexical(it->path(), base);
        if (options.use_gitignore && gitignore.ignored(rel)) continue;
        entries.push_back(make_symlink_entry(it->path(), base));
      } else if ((is_symlink && std::filesystem::is_regular_file(it->path(), ec)) ||
                 (!is_symlink && it->is_regular_file(ec))) {
        if (ec) {
          ec.clear();
          continue;
        }
        auto rel = relative_lexical(it->path(), base);
        if (options.use_gitignore && gitignore.ignored(rel)) continue;
        const auto size = std::filesystem::file_size(it->path(), ec);
        if (ec) {
          ec.clear();
          continue;
        }
        FileEntry entry;
        entry.absolute = it->path();
        entry.relative = rel;
        entry.size = size;
        entry.imohash = imohash_hex(entry.absolute);
        entry.mtime_ms = detail::file_mtime_ms(entry.absolute);
        entry.mode = detail::file_mode_bits(entry.absolute);
        entries.push_back(std::move(entry));
      } else if (!is_symlink && it->is_directory(ec)) {
        if (ec) {
          ec.clear();
          continue;
        }
        std::filesystem::directory_iterator dir_it(it->path(), ec);
        if (ec || dir_it != std::filesystem::directory_iterator()) continue;
        auto rel = relative_lexical(it->path(), base) + "/";
        if (options.use_gitignore && gitignore.ignored(rel.substr(0, rel.size() - 1))) continue;
        FileEntry entry;
        entry.absolute = it->path();
        entry.relative = rel;
        entry.mtime_ms = detail::file_mtime_ms(entry.absolute);
        entry.mode = detail::file_mode_bits(entry.absolute);
        entries.push_back(std::move(entry));
      }
    }
    if (entries.empty()) {
      auto rel = path.filename().string() + "/";
      FileEntry entry;
      entry.absolute = path;
      entry.relative = rel;
      entry.mtime_ms = detail::file_mtime_ms(path);
      entry.mode = detail::file_mode_bits(path);
      entries.push_back(std::move(entry));
    }
  } else if (std::filesystem::is_regular_file(path, ec)) {
    FileEntry entry;
    entry.absolute = path;
    entry.relative = path.filename().string();
    entry.size = std::filesystem::file_size(path);
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
