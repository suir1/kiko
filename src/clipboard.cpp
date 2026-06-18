#include "clipboard.hpp"

#include <cstdio>

namespace kiko {

bool copy_to_clipboard(const std::string& text) {
  if (text.empty()) return false;

#if defined(__APPLE__)
  FILE* pipe = popen("pbcopy", "w");
  if (!pipe) return false;
  const auto written = std::fwrite(text.data(), 1, text.size(), pipe);
  return pclose(pipe) == 0 && written == text.size();
#elif defined(_WIN32)
  (void)text;
  return false;
#else
  const char* commands[] = {"wl-copy", "xclip -selection clipboard"};
  for (const char* cmd : commands) {
    FILE* pipe = popen(cmd, "w");
    if (!pipe) continue;
    const auto written = std::fwrite(text.data(), 1, text.size(), pipe);
    if (pclose(pipe) == 0 && written == text.size()) return true;
  }
  return false;
#endif
}

}  // namespace kiko
