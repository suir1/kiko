#include "transfer.hpp"

#include "file_metadata.hpp"
#include "transfer/gitignore.hpp"
#include "core/imohash.hpp"
#include "transfer_receive_paths.hpp"

#include <algorithm>

namespace kiko {
namespace {

std::string relative_lexical(const std::filesystem::path& path, const std::filesystem::path& base) {
  auto rel = path.lexically_relative(base);
  if (rel.empty()) rel = std::filesystem::relative(path, base);
  return rel.generic_string();
}

FileEntry make_symlink_entry(const std::filesystem::path& path, const std::filesystem::path& base) {
  std::error_code ec;
  const auto target = std::filesystem::read_symlink(path, ec);
  if (ec) throw KikoError("failed to read symlink: " + path.string());
  if (!detail::is_safe_relative_symlink_target(target)) {
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
    const bool follow_directory_symlinks = options.symlink_mode == SymlinkMode::Follow;
    const auto directory_options = follow_directory_symlinks
                                       ? std::filesystem::directory_options::follow_directory_symlink
                                       : std::filesystem::directory_options::none;
    std::vector<std::filesystem::path> canonical_ancestors;
    if (follow_directory_symlinks) {
      const auto canonical_root = std::filesystem::weakly_canonical(path, ec);
      if (ec) throw KikoError("failed to resolve source directory: " + path.string() + ": " + ec.message());
      canonical_ancestors.push_back(canonical_root);
    }
    for (auto it = std::filesystem::recursive_directory_iterator(path, directory_options);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
      if (follow_directory_symlinks) {
        canonical_ancestors.resize(std::min(canonical_ancestors.size(),
                                            static_cast<std::size_t>(it.depth()) + 1));
      }
      const bool is_symlink = it->is_symlink(ec);
      if (ec) {
        ec.clear();
        continue;
      }
      const bool is_directory = (!is_symlink || follow_directory_symlinks) && it->is_directory(ec);
      if (ec) {
        ec.clear();
        continue;
      }
      if (follow_directory_symlinks && is_directory) {
        const auto canonical_directory = std::filesystem::weakly_canonical(it->path(), ec);
        if (ec) {
          throw KikoError("failed to resolve source directory: " + it->path().string() + ": " + ec.message());
        }
        if (std::find(canonical_ancestors.begin(), canonical_ancestors.end(), canonical_directory) !=
            canonical_ancestors.end()) {
          it.disable_recursion_pending();
          continue;
        }
        canonical_ancestors.push_back(canonical_directory);
      }
      if (options.use_gitignore && gitignore.ignored(it->path(), is_directory)) {
        if (is_directory) it.disable_recursion_pending();
        continue;
      }
      if (options.use_gitignore && is_directory) {
        const auto nested_ignore = it->path() / ".gitignore";
        if (std::filesystem::is_regular_file(nested_ignore, ec) && !ec) gitignore.add_file(nested_ignore);
        ec.clear();
      }
      if (options.symlink_mode == SymlinkMode::Preserve && is_symlink) {
        auto rel = relative_lexical(it->path(), base);
        entries.push_back(make_symlink_entry(it->path(), base));
      } else if ((is_symlink && std::filesystem::is_regular_file(it->path(), ec)) ||
                 (!is_symlink && it->is_regular_file(ec))) {
        if (ec) {
          ec.clear();
          continue;
        }
        auto rel = relative_lexical(it->path(), base);
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
      } else if (is_directory) {
        std::filesystem::directory_iterator dir_it(it->path(), ec);
        if (ec || dir_it != std::filesystem::directory_iterator()) continue;
        auto rel = relative_lexical(it->path(), base) + "/";
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
