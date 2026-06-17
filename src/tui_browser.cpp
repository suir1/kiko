#include "tui_browser.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <filesystem>
#include <vector>

namespace kiko {
namespace {

struct BrowserEntry {
  std::filesystem::path path;
  bool is_dir = false;
  std::string label;
};

std::filesystem::path normalize_dir(const std::filesystem::path& path) {
  std::error_code ec;
  auto abs = std::filesystem::absolute(path, ec);
  if (ec) return path;
  if (std::filesystem::is_directory(abs, ec) && !ec) return abs;
  return abs.parent_path();
}

std::vector<BrowserEntry> list_entries(const std::filesystem::path& dir, TuiPickMode mode) {
  std::vector<BrowserEntry> out;
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec) || ec) return out;

  if (dir.has_parent_path()) {
    BrowserEntry parent;
    parent.path = dir.parent_path();
    parent.is_dir = true;
    parent.label = "../";
    out.push_back(std::move(parent));
  }

  std::vector<BrowserEntry> dirs;
  std::vector<BrowserEntry> files;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (ec) break;
    BrowserEntry item;
    item.path = entry.path();
    item.is_dir = entry.is_directory(ec);
    const auto name = entry.path().filename().string();
    if (name.empty() || name == ".") continue;
    if (item.is_dir) {
      item.label = name + "/";
      dirs.push_back(std::move(item));
    } else if (mode == TuiPickMode::FileOrDirectory) {
      item.label = name;
      files.push_back(std::move(item));
    }
  }

  auto by_name = [](const BrowserEntry& a, const BrowserEntry& b) { return a.label < b.label; };
  std::sort(dirs.begin(), dirs.end(), by_name);
  std::sort(files.begin(), files.end(), by_name);
  out.insert(out.end(), dirs.begin(), dirs.end());
  out.insert(out.end(), files.begin(), files.end());

  BrowserEntry pick_here;
  pick_here.path = dir;
  pick_here.is_dir = true;
  pick_here.label = mode == TuiPickMode::DirectoryOnly ? "[Select this folder]" : "[Select this folder]";
  out.push_back(std::move(pick_here));

  return out;
}

}  // namespace

std::optional<std::filesystem::path> run_tui_path_picker(const std::filesystem::path& start, TuiPickMode mode) {
  using namespace ftxui;

  std::filesystem::path current = normalize_dir(start);
  std::vector<BrowserEntry> entries = list_entries(current, mode);
  std::vector<std::string> labels;
  labels.reserve(entries.size());
  for (const auto& entry : entries) labels.push_back(entry.label);

  int selected = 0;
  bool cancelled = false;
  std::optional<std::filesystem::path> result;

  auto refresh = [&] {
    entries = list_entries(current, mode);
    labels.clear();
    labels.reserve(entries.size());
    for (const auto& entry : entries) labels.push_back(entry.label);
    if (selected >= static_cast<int>(labels.size())) selected = static_cast<int>(labels.size()) - 1;
    if (selected < 0) selected = 0;
  };

  auto menu = Menu(&labels, &selected);
  auto screen = ScreenInteractive::Fullscreen();

  auto layout = Renderer(menu, [&] {
    Elements rows;
    rows.push_back(text("Select path") | bold | hcenter);
    rows.push_back(text(current.string()) | dim);
    rows.push_back(separator());
    rows.push_back(menu->Render() | frame | size(HEIGHT, LESS_THAN, 20) | flex);
    rows.push_back(separator());
  rows.push_back(text(mode == TuiPickMode::DirectoryOnly ? "Enter: open folder / select  Esc: cancel"
                                                         : "Enter: open or select file  Esc: cancel") |
                 dim);
    return vbox(std::move(rows)) | border;
  });

  auto on_activate = [&] {
    if (entries.empty()) return;
    const auto& entry = entries[static_cast<std::size_t>(selected)];
    if (entry.label == "../") {
      current = entry.path;
      refresh();
      return;
    }
    if (entry.label == "[Select this folder]") {
      result = current;
      screen.Exit();
      return;
    }
    if (entry.is_dir) {
      current = entry.path;
      refresh();
      return;
    }
    result = entry.path;
    screen.Exit();
  };

  auto with_events = CatchEvent(layout, [&](Event event) {
    if (event == Event::Escape || event == Event::Character('q')) {
      cancelled = true;
      screen.Exit();
      return true;
    }
    if (event == Event::Return) {
      on_activate();
      return true;
    }
    return false;
  });

  screen.Loop(with_events);
  if (cancelled || !result) return std::nullopt;
  return *result;
}

}  // namespace kiko
