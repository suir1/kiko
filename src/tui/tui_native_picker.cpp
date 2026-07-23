#include "tui_native_picker.hpp"

namespace kiko {

bool pick_tui_native_path(std::string& target, NativePickMode mode,
                          std::string& menu_error) {
  const auto result = pick_native_path(mode);
  switch (result.status) {
    case NativePickStatus::Selected:
      target = result.path.string();
      menu_error.clear();
      return true;
    case NativePickStatus::Canceled:
      menu_error.clear();
      return false;
    case NativePickStatus::Unavailable:
    case NativePickStatus::Error:
      menu_error = result.error.empty() ? "system picker is unavailable" : result.error;
      return false;
  }
  menu_error = "system picker returned an unknown result";
  return false;
}

}  // namespace kiko
