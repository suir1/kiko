#include "web/web.hpp"
#include "web/web_assets.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

namespace {

fs::path make_temp_root() {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  return fs::temp_directory_path() / ("kiko_web_test_" + std::to_string(stamp));
}

std::size_t find_label(const std::vector<kiko::WebDirectoryEntry>& entries, const std::string& label) {
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].label == label) return i;
  }
  return entries.size();
}

void set_modified_time(const fs::path& path, fs::file_time_type time) {
  std::error_code ec;
  fs::last_write_time(path, time, ec);
  if (ec) {
    std::cerr << "FAIL: could not set modified time\n";
    std::exit(1);
  }
}

}  // namespace

int main() {
  using namespace kiko;

  assert(web_listen_is_loopback(Endpoint{"127.0.0.1", 0}));
  assert(web_listen_is_loopback(Endpoint{"::1", 0}));
  assert(web_listen_is_loopback(Endpoint{"localhost", 0}));
  assert(!web_listen_is_loopback(Endpoint{"0.0.0.0", 0}));
  assert(!web_listen_is_loopback(Endpoint{"192.168.1.10", 0}));

  const auto token_a = generate_web_token();
  const auto token_b = generate_web_token();
  assert(token_a.size() == 48);
  assert(token_b.size() == 48);
  assert(token_a != token_b);
  const auto html = std::string(web_index_html());
  assert(html.find("Notepad") != std::string::npos);
  assert(html.find("/api/note/start") != std::string::npos);
  assert(html.find("/api/note/update") != std::string::npos);
  assert(html.find("noteEditGeneration") != std::string::npos);
  assert(html.find("generation === noteEditGeneration") != std::string::npos);

  const auto root = make_temp_root();
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / "older_dir");
  fs::create_directories(root / "newer_dir");
  std::ofstream(root / "a.txt") << "a\n";
  std::ofstream(root / "z.txt") << "z\n";

  const auto now = fs::file_time_type::clock::now();
  set_modified_time(root / "a.txt", now - std::chrono::hours(2));
  set_modified_time(root / "z.txt", now - std::chrono::minutes(5));

  const auto send_entries = list_web_directory(root, WebPickMode::FileOrDirectory, WebBrowserSort::Name);
  const auto a_pos = find_label(send_entries, "a.txt");
  const auto z_pos = find_label(send_entries, "z.txt");
  const auto dir_pos = find_label(send_entries, "newer_dir/");
  assert(a_pos != send_entries.size());
  assert(z_pos != send_entries.size());
  assert(dir_pos != send_entries.size());
  assert(a_pos < dir_pos);
  assert(z_pos < dir_pos);
  assert(find_label(send_entries, "[Select this folder]") != send_entries.size());

  const auto recent_entries = list_web_directory(root, WebPickMode::FileOrDirectory, WebBrowserSort::ModifiedDesc);
  assert(find_label(recent_entries, "z.txt") < find_label(recent_entries, "a.txt"));

  const auto dir_entries = list_web_directory(root, WebPickMode::DirectoryOnly, WebBrowserSort::Name);
  assert(find_label(dir_entries, "a.txt") == dir_entries.size());
  assert(find_label(dir_entries, "newer_dir/") != dir_entries.size());

  const auto filtered = list_web_directory(root, WebPickMode::FileOrDirectory, WebBrowserSort::Name, "z.");
  assert(find_label(filtered, "z.txt") != filtered.size());
  assert(find_label(filtered, "a.txt") == filtered.size());

  fs::remove_all(root, ec);
  std::cout << "web_test ok\n";
  return 0;
}
