#include "transfer/transfer.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>

namespace fs = std::filesystem;
using namespace kiko;

namespace {

struct TempTree {
  fs::path path;

  ~TempTree() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

void write_file(const fs::path& path, const std::string& contents = "data\n") {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << contents;
}

std::set<std::string> relative_paths(const std::vector<FileEntry>& entries) {
  std::set<std::string> paths;
  for (const auto& entry : entries) paths.insert(entry.relative);
  return paths;
}

bool require_path(const std::set<std::string>& paths, const std::string& path, bool expected) {
  const bool present = paths.contains(path);
  if (present == expected) return true;
  std::cerr << "FAIL: expected " << path << (expected ? " in" : " absent from") << " send manifest\n";
  return false;
}

}  // namespace

int main() {
  const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
  TempTree temp{fs::temp_directory_path() / ("kiko-gitignore-test-" + std::to_string(suffix))};
  const auto root = temp.path / "project";

  write_file(root / ".gitignore",
             "build/\n"
             "*.log\n"
             "!important.log\n"
             "/anchored.txt\n"
             "cache/**/tmp/\n"
             "artifact-[0-9].bin\n");
  write_file(root / "keep.txt");
  write_file(root / "build" / "object.o");
  write_file(root / "debug.log");
  write_file(root / "important.log");
  write_file(root / "anchored.txt");
  write_file(root / "sub" / "anchored.txt");
  write_file(root / "sub" / "debug.log");
  write_file(root / "cache" / "tmp" / "one.dat");
  write_file(root / "cache" / "a" / "b" / "tmp" / "two.dat");
  write_file(root / "artifact-7.bin");
  write_file(root / "artifact-x.bin");
  write_file(root / "src" / ".gitignore",
             "generated/\n"
             "*.tmp\n"
             "!keep.tmp\n");
  write_file(root / "src" / "generated" / "code.bin");
  write_file(root / "src" / "drop.tmp");
  write_file(root / "src" / "keep.tmp");
  write_file(root / "src" / "main.cpp");

  const auto filtered = relative_paths(collect_files(root));
  bool ok = true;
  ok &= require_path(filtered, "project/keep.txt", true);
  ok &= require_path(filtered, "project/important.log", true);
  ok &= require_path(filtered, "project/sub/anchored.txt", true);
  ok &= require_path(filtered, "project/src/keep.tmp", true);
  ok &= require_path(filtered, "project/src/main.cpp", true);
  ok &= require_path(filtered, "project/artifact-x.bin", true);
  ok &= require_path(filtered, "project/build/object.o", false);
  ok &= require_path(filtered, "project/debug.log", false);
  ok &= require_path(filtered, "project/anchored.txt", false);
  ok &= require_path(filtered, "project/sub/debug.log", false);
  ok &= require_path(filtered, "project/src/generated/code.bin", false);
  ok &= require_path(filtered, "project/src/drop.tmp", false);
  ok &= require_path(filtered, "project/cache/tmp/one.dat", false);
  ok &= require_path(filtered, "project/cache/a/b/tmp/two.dat", false);
  ok &= require_path(filtered, "project/artifact-7.bin", false);

  CollectOptions unfiltered_options;
  unfiltered_options.use_gitignore = false;
  const auto unfiltered = relative_paths(collect_files(root, unfiltered_options));
  ok &= require_path(unfiltered, "project/build/object.o", true);
  ok &= require_path(unfiltered, "project/debug.log", true);
  ok &= require_path(unfiltered, "project/src/generated/code.bin", true);
  ok &= require_path(unfiltered, "project/src/drop.tmp", true);

  const auto repository = temp.path / "repository";
  fs::create_directories(repository / ".git");
  write_file(repository / ".gitignore", "selected/from-parent.txt\n");
  write_file(repository / "selected" / "from-parent.txt");
  write_file(repository / "selected" / "kept.txt");
  const auto selected = relative_paths(collect_files(repository / "selected"));
  ok &= require_path(selected, "selected/from-parent.txt", false);
  ok &= require_path(selected, "selected/kept.txt", true);

  if (!ok) return 1;
  std::cout << "gitignore ok\n";
  return 0;
}
