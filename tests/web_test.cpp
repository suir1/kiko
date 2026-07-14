#include "web/web.hpp"
#include "web/web_assets.hpp"
#include "platform/path_browser.hpp"

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

std::size_t find_label(const std::vector<kiko::PathBrowserEntry>& entries, const std::string& label) {
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
  assert(html.find("Start notepad") != std::string::npos);
  assert(html.find("Custom code host") != std::string::npos);
  assert(html.find("note-custom-host") != std::string::npos);
  assert(html.find("noteRole = code && !qs('note-custom-host').checked ? 'join' : 'host'") != std::string::npos);
  assert(html.find("Copy code") != std::string::npos);
  assert(html.find("Copy note") != std::string::npos);
  assert(html.find("New note") != std::string::npos);
  assert(html.find("note-pads") != std::string::npos);
  assert(html.find("showNoteQr") != std::string::npos);
  assert(html.find("/api/qr") != std::string::npos);
  assert(html.find("/api/note/pad/create") != std::string::npos);
  assert(html.find("/api/note/pad/select") != std::string::npos);
  assert(html.find("renderNotePads") != std::string::npos);
  assert(html.find("QR contains the note text directly") != std::string::npos);
  assert(html.find("Host notepad") == std::string::npos);
  assert(html.find("Join notepad") == std::string::npos);
  assert(html.find("note-role") == std::string::npos);
  assert(html.find("/api/note/start") != std::string::npos);
  assert(html.find("/api/note/update") != std::string::npos);
  assert(html.find("noteEditGeneration") != std::string::npos);
  assert(html.find("generation === noteEditGeneration") != std::string::npos);
  assert(html.find("kiko.web.path.") != std::string::npos);
  assert(html.find("kiko.web.browser.") != std::string::npos);
  assert(html.find("terminalActivity") != std::string::npos);
  assert(html.find("Canceling...") != std::string::npos);

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

  const auto send_entries =
      browse_directory(root, PathPickMode::FileOrDirectory, PathBrowserSort::Name);
  const auto a_pos = find_label(send_entries, "a.txt");
  const auto z_pos = find_label(send_entries, "z.txt");
  const auto dir_pos = find_label(send_entries, "newer_dir/");
  assert(a_pos != send_entries.size());
  assert(z_pos != send_entries.size());
  assert(dir_pos != send_entries.size());
  assert(a_pos < dir_pos);
  assert(z_pos < dir_pos);
  assert(find_label(send_entries, "[Select this folder]") != send_entries.size());

  const auto recent_entries =
      browse_directory(root, PathPickMode::FileOrDirectory, PathBrowserSort::ModifiedDesc);
  assert(find_label(recent_entries, "z.txt") < find_label(recent_entries, "a.txt"));

  const auto dir_entries = browse_directory(root, PathPickMode::DirectoryOnly, PathBrowserSort::Name);
  assert(find_label(dir_entries, "a.txt") == dir_entries.size());
  assert(find_label(dir_entries, "newer_dir/") != dir_entries.size());

  const auto filtered =
      browse_directory(root, PathPickMode::FileOrDirectory, PathBrowserSort::Name, "z.");
  assert(find_label(filtered, "z.txt") != filtered.size());
  assert(find_label(filtered, "a.txt") == filtered.size());

  fs::remove_all(root, ec);
  std::cout << "web_test ok\n";
  return 0;
}
