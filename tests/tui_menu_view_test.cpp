#include "tui/tui_menu_view.hpp"

#include <iostream>

int main() {
  int send_browse_count = 0;
  int output_browse_count = 0;
  const auto send_browse = [&] { ++send_browse_count; };
  const auto output_browse = [&] { ++output_browse_count; };

  kiko::detail::invoke_active_browse_callback(0, send_browse, output_browse);
  if (send_browse_count != 1 || output_browse_count != 0) {
    std::cerr << "FAIL: send mode should browse files/directories, not output directories\n";
    return 1;
  }

  kiko::detail::invoke_active_browse_callback(1, send_browse, output_browse);
  if (send_browse_count != 1 || output_browse_count != 1) {
    std::cerr << "FAIL: receive mode should browse output directories\n";
    return 1;
  }

  std::cout << "tui_menu_view_test ok\n";
  return 0;
}
