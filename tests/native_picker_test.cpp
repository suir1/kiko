#include "platform/native_picker.hpp"
#include "tui/tui_native_picker.hpp"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

namespace {

void set_env(const char *key, const std::string &value) {
#ifdef _WIN32
  _putenv_s(key, value.c_str());
#else
  setenv(key, value.c_str(), 1);
#endif
}

void clear_env(const char *key) {
#ifdef _WIN32
  _putenv_s(key, "");
#else
  unsetenv(key);
#endif
}

} // namespace

int main() {
  using namespace kiko;

  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto root = fs::temp_directory_path() /
                    ("kiko_native_picker_test_" + std::to_string(stamp));
  const auto file = root / "picked.txt";
  const auto dir = root / "picked-dir";
  std::error_code ec;
  fs::create_directories(dir);
  std::ofstream(file) << "picked\n";

  set_env("KIKO_TEST_NATIVE_PICK_FILE", file.string());
  set_env("KIKO_TEST_NATIVE_PICK_DIR", dir.string());

  const auto picked_file = pick_native_path(NativePickMode::File);
  assert(picked_file.status == NativePickStatus::Selected);
  assert(picked_file.path == file);

  const auto picked_dir = pick_native_path(NativePickMode::Directory);
  assert(picked_dir.status == NativePickStatus::Selected);
  assert(picked_dir.path == dir);

  std::string tui_path = "unchanged";
  std::string tui_error = "old error";
  assert(pick_tui_native_path(tui_path, NativePickMode::File, tui_error));
  assert(tui_path == file.string());
  assert(tui_error.empty());

  assert(pick_tui_native_path(tui_path, NativePickMode::Directory, tui_error));
  assert(tui_path == dir.string());
  assert(tui_error.empty());

  set_env("KIKO_TEST_NATIVE_PICK_FILE", dir.string());
  const auto wrong_type = pick_native_path(NativePickMode::File);
  assert(wrong_type.status == NativePickStatus::Error);

  clear_env("KIKO_TEST_NATIVE_PICK_FILE");
  clear_env("KIKO_TEST_NATIVE_PICK_DIR");
  fs::remove_all(root, ec);
  std::cout << "native picker ok\n";
  return 0;
}
