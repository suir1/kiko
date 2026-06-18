#include "tui_browser.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

namespace kiko {
namespace {

struct BrowserEntry {
  std::filesystem::path path;
  bool is_dir = false;
  std::string label;
};

constexpr const char* kParentLabel = "../";
constexpr const char* kSelectHereLabel = "[Select this folder]";

std::filesystem::path normalize_dir(const std::filesystem::path& path) {
  std::error_code ec;
  auto abs = std::filesystem::absolute(path, ec);
  if (ec) return path;
  if (std::filesystem::is_directory(abs, ec) && !ec) return abs;
  return abs.parent_path();
}

std::string to_lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool is_special_entry(const std::string& label) { return label == kParentLabel || label == kSelectHereLabel; }

std::string entry_match_name(const std::string& label) {
  if (label.size() > 1 && label.back() == '/') return label.substr(0, label.size() - 1);
  return label;
}

bool matches_filter_ci(const std::string& label, const std::string& filter) {
  if (filter.empty()) return true;
  return to_lower(entry_match_name(label)).find(to_lower(filter)) != std::string::npos;
}

std::vector<BrowserEntry> list_entries(const std::filesystem::path& dir, TuiPickMode mode) {
  std::vector<BrowserEntry> out;
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec) || ec) return out;

  if (dir.has_parent_path()) {
    BrowserEntry parent;
    parent.path = dir.parent_path();
    parent.is_dir = true;
    parent.label = kParentLabel;
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
  pick_here.label = kSelectHereLabel;
  out.push_back(std::move(pick_here));

  return out;
}

std::vector<BrowserEntry> apply_filter(const std::vector<BrowserEntry>& all, const std::string& filter) {
  if (filter.empty()) return all;

  std::vector<BrowserEntry> out;
  out.reserve(all.size());
  for (const auto& entry : all) {
    if (is_special_entry(entry.label) || matches_filter_ci(entry.label, filter)) {
      out.push_back(entry);
    }
  }
  return out;
}

}  // namespace

std::optional<std::filesystem::path> run_tui_path_picker(const std::filesystem::path& start, TuiPickMode mode) {
  using namespace ftxui;

  std::filesystem::path current = normalize_dir(start);
  std::vector<BrowserEntry> all_entries = list_entries(current, mode);
  std::vector<BrowserEntry> entries;
  std::vector<std::string> labels;
  std::string filter;
  std::string synced_filter;
  std::filesystem::path synced_dir;

  int selected = 0;
  bool cancelled = false;
  std::optional<std::filesystem::path> result;

  auto sync_visible = [&] {
    if (filter == synced_filter && current == synced_dir && !labels.empty()) return;

    synced_filter = filter;
    synced_dir = current;
    entries = apply_filter(all_entries, filter);
    labels.clear();
    labels.reserve(entries.size());
    for (const auto& entry : entries) labels.push_back(entry.label);
    if (labels.empty()) {
      selected = 0;
      return;
    }
    if (selected >= static_cast<int>(labels.size())) selected = static_cast<int>(labels.size()) - 1;
    if (selected < 0) selected = 0;
  };

  auto reload_directory = [&]() {
    all_entries = list_entries(current, mode);
    filter.clear();
    synced_filter.clear();
    synced_dir.clear();
    selected = 0;
    sync_visible();
  };

  sync_visible();

  InputOption filter_opt;
  filter_opt.multiline = false;
  auto filter_input = Input(&filter, "filter name…", filter_opt);
  auto menu = Menu(&labels, &selected);
  auto browser_layout = Container::Vertical({filter_input, menu});
  auto screen = ScreenInteractive::Fullscreen();

  auto layout = Renderer(browser_layout, [&] {
    sync_visible();

    Elements rows;
    rows.push_back(text("Select path") | bold | hcenter);
    rows.push_back(text(current.string()) | dim);
    rows.push_back(hbox({text("filter: "), filter_input->Render() | flex}));
    rows.push_back(separator());

    std::size_t match_count = 0;
    for (const auto& entry : entries) {
      if (!is_special_entry(entry.label)) ++match_count;
    }

    if (!filter.empty() && match_count == 0) {
      rows.push_back(text("  (no matches in this folder)") | dim);
      rows.push_back(text("  ../ and [Select this folder] stay available below") | dim);
    }

    if (!labels.empty()) {
      rows.push_back(menu->Render() | frame | size(HEIGHT, LESS_THAN, 20) | flex);
    }
    if (!filter.empty() && match_count > 0) {
      rows.push_back(text(std::to_string(match_count) + " match(es)") | dim);
    }

    rows.push_back(separator());
    rows.push_back(text(mode == TuiPickMode::DirectoryOnly ? "Tab: filter/list  Enter: open/select  Esc: cancel"
                                                           : "Tab: filter/list  Enter: open/select  Esc: cancel") |
                   dim);
    return vbox(std::move(rows)) | border;
  });

  auto on_activate = [&] {
    sync_visible();
    if (entries.empty()) return;
    const auto& entry = entries[static_cast<std::size_t>(selected)];
    if (entry.label == kParentLabel) {
      current = entry.path;
      reload_directory();
      return;
    }
    if (entry.label == kSelectHereLabel) {
      result = current;
      screen.Exit();
      return;
    }
    if (entry.is_dir) {
      current = entry.path;
      reload_directory();
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
