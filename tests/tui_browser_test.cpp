#include "tui/tui_browser.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::size_t find_label(const std::vector<std::string>& labels, const std::string& needle) {
  for (std::size_t i = 0; i < labels.size(); ++i) {
    if (labels[i] == needle) return i;
  }
  return labels.size();
}

bool contains_label(const std::vector<std::string>& labels, const std::string& needle) {
  return find_label(labels, needle) != labels.size();
}

void set_modified_time(const fs::path& path, fs::file_time_type time) {
  std::error_code ec;
  fs::last_write_time(path, time, ec);
  if (ec) {
    std::cerr << "FAIL: could not set modified time for " << path << "\n";
    std::exit(1);
  }
}

fs::path make_temp_root() {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  return fs::temp_directory_path() / ("kiko_tui_browser_test_" + std::to_string(stamp));
}

}  // namespace

int main() {
  using namespace kiko;

  const auto root = make_temp_root();
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root);

  for (int i = 0; i < 30; ++i) {
    fs::create_directories(root / ("dir" + std::to_string(100 + i)));
  }
  {
    std::ofstream(root / "a.txt") << "a\n";
    std::ofstream(root / "z.txt") << "z\n";
  }
  const auto now = fs::file_time_type::clock::now();
  set_modified_time(root / "a.txt", now - std::chrono::hours(2));
  set_modified_time(root / "z.txt", now - std::chrono::minutes(5));

  const auto send_labels = detail::list_tui_path_picker_labels(root, TuiPickMode::FileOrDirectory);
  const auto a_pos = find_label(send_labels, "a.txt");
  const auto z_pos = find_label(send_labels, "z.txt");
  const auto first_dir_pos = find_label(send_labels, "dir100/");
  if (a_pos == send_labels.size() || z_pos == send_labels.size() || first_dir_pos == send_labels.size()) {
    std::cerr << "FAIL: send picker did not list expected files and directories\n";
    fs::remove_all(root, ec);
    return 1;
  }
  if (!(a_pos < first_dir_pos && z_pos < first_dir_pos)) {
    std::cerr << "FAIL: send picker should show files before directories\n";
    fs::remove_all(root, ec);
    return 1;
  }
  if (!contains_label(send_labels, "[Select this folder]")) {
    std::cerr << "FAIL: send picker should allow selecting the current folder\n";
    fs::remove_all(root, ec);
    return 1;
  }

  const auto recent_labels =
      detail::list_tui_path_picker_labels(root, TuiPickMode::FileOrDirectory, TuiBrowserSort::ModifiedDesc);
  if (!(find_label(recent_labels, "z.txt") < find_label(recent_labels, "a.txt"))) {
    std::cerr << "FAIL: modified-time sort should show newer files first\n";
    fs::remove_all(root, ec);
    return 1;
  }

  const auto receive_labels = detail::list_tui_path_picker_labels(root, TuiPickMode::DirectoryOnly);
  if (contains_label(receive_labels, "a.txt") || !contains_label(receive_labels, "dir100/") ||
      !contains_label(receive_labels, "[Select this folder]")) {
    std::cerr << "FAIL: directory picker should list directories and omit files\n";
    fs::remove_all(root, ec);
    return 1;
  }

  fs::remove_all(root, ec);
  std::cout << "tui_browser_test ok\n";
  return 0;
}
