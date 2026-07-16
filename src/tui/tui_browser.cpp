#include "tui_browser.hpp"

#include "core/common.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace kiko {
namespace {

std::vector<PathBrowserEntry> list_entries(const std::filesystem::path& dir, PathPickMode mode,
                                           PathBrowserSort sort) {
  try {
    return list_browser_directory(dir, mode, sort);
  } catch (const KikoError&) {
    return {};
  }
}

}  // namespace

std::optional<std::filesystem::path> run_tui_path_picker(const std::filesystem::path& start, PathPickMode mode) {
  using namespace ftxui;

  std::filesystem::path current = normalize_browser_directory(start);
  PathBrowserSort sort_mode = PathBrowserSort::Name;
  std::vector<PathBrowserEntry> all_entries = list_entries(current, mode, sort_mode);
  std::vector<PathBrowserEntry> entries;
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
    entries = filter_browser_entries(all_entries, filter);
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

  auto reload_entries = [&]() {
    all_entries = list_entries(current, mode, sort_mode);
    synced_filter.clear();
    synced_dir.clear();
    selected = 0;
    sync_visible();
  };

  auto reload_directory = [&]() {
    filter.clear();
    reload_entries();
  };

  sync_visible();

  InputOption filter_opt;
  filter_opt.multiline = false;
  auto filter_input = Input(&filter, "filter name…", filter_opt);
  auto menu = Menu(&labels, &selected);
  auto sort_name = Button("Name", [&] {
    sort_mode = PathBrowserSort::Name;
    reload_entries();
  });
  auto sort_modified = Button("Modified", [&] {
    sort_mode = PathBrowserSort::ModifiedDesc;
    reload_entries();
  });
  auto sort_controls = Container::Horizontal({sort_name, sort_modified});
  auto browser_layout = Container::Vertical({filter_input, sort_controls, menu});
  auto screen = ScreenInteractive::Fullscreen();

  auto layout = Renderer(browser_layout, [&] {
    sync_visible();

    Elements rows;
    rows.push_back(text("Select path") | bold | hcenter);
    rows.push_back(text(current.string()) | dim);
    rows.push_back(hbox({text("filter: "), filter_input->Render() | flex}));
    rows.push_back(hbox({text("sort:   "), sort_name->Render(), text(" "), sort_modified->Render(),
                         text(sort_mode == PathBrowserSort::Name ? "  current=name" : "  current=modified desc") |
                             dim}));
    rows.push_back(separator());

    std::size_t match_count = 0;
    for (const auto& entry : entries) {
      if (!entry.parent && !entry.select_here) ++match_count;
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
    rows.push_back(text("Tab: filter/sort/list  Enter: open/select  Esc: cancel") | dim);
    return vbox(std::move(rows)) | border;
  });

  auto on_activate = [&] {
    sync_visible();
    if (entries.empty()) return;
    const auto& entry = entries[static_cast<std::size_t>(selected)];
    if (entry.parent) {
      current = entry.path;
      reload_directory();
      return;
    }
    if (entry.select_here) {
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
