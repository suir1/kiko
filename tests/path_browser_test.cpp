#include "platform/path_browser.hpp"

#include "core/common.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

namespace {

fs::path make_temp_root() {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  return fs::temp_directory_path() / ("kiko_path_browser_test_" + std::to_string(stamp));
}

std::size_t find_label(const std::vector<kiko::PathBrowserEntry>& entries, const std::string& label) {
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].label == label) return i;
  }
  return entries.size();
}

void set_modified_time(const fs::path& path, fs::file_time_type time) {
  std::error_code ec;
  fs::last_write_time(path, time, ec);
  assert(!ec);
}

}  // namespace

int main() {
  using namespace kiko;

  const auto root = make_temp_root();
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / "alpha_dir");
  fs::create_directories(root / "zeta_dir");
  std::ofstream(root / "a.txt") << "a\n";
  std::ofstream(root / "Z.txt") << "z\n";

  const auto now = fs::file_time_type::clock::now();
  set_modified_time(root / "a.txt", now - std::chrono::hours(2));
  set_modified_time(root / "Z.txt", now - std::chrono::minutes(5));

  const auto all = list_browser_directory(root, PathPickMode::FileOrDirectory, PathBrowserSort::Name);
  assert(find_label(all, "../") == 0);
  assert(find_label(all, "a.txt") < find_label(all, "alpha_dir/"));
  assert(find_label(all, "Z.txt") < find_label(all, "alpha_dir/"));
  assert(find_label(all, "[Select this folder]") == all.size() - 1);

  const auto recent =
      list_browser_directory(root, PathPickMode::FileOrDirectory, PathBrowserSort::ModifiedDesc);
  assert(find_label(recent, "Z.txt") < find_label(recent, "a.txt"));

  const auto dirs = list_browser_directory(root, PathPickMode::DirectoryOnly, PathBrowserSort::Name);
  assert(find_label(dirs, "a.txt") == dirs.size());
  assert(find_label(dirs, "alpha_dir/") != dirs.size());

  const auto filtered = filter_browser_entries(all, "z.");
  assert(find_label(filtered, "../") != filtered.size());
  assert(find_label(filtered, "Z.txt") != filtered.size());
  assert(find_label(filtered, "a.txt") == filtered.size());
  assert(find_label(filtered, "[Select this folder]") != filtered.size());

  assert(normalize_browser_directory(root / "a.txt") == fs::absolute(root));

  bool rejected = false;
  try {
    (void)list_browser_directory(root / "missing", PathPickMode::FileOrDirectory, PathBrowserSort::Name);
  } catch (const KikoError&) {
    rejected = true;
  }
  assert(rejected);

  fs::remove_all(root, ec);
  std::cout << "path browser ok\n";
  return 0;
}
