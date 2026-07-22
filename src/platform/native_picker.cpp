#include "platform/native_picker.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace kiko {
namespace {

struct CommandResult {
  bool started = false;
  int status = -1;
  std::string output;
};

FILE *open_pipe(const char *command) {
#ifdef _WIN32
  return _popen(command, "r");
#else
  return popen(command, "r");
#endif
}

int close_pipe(FILE *pipe) {
#ifdef _WIN32
  return _pclose(pipe);
#else
  return pclose(pipe);
#endif
}

CommandResult run_command(const std::string &command) {
  CommandResult result;
  FILE *pipe = open_pipe(command.c_str());
  if (!pipe)
    return result;
  result.started = true;
  std::array<char, 1024> buffer{};
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe))
    result.output += buffer.data();
  result.status = close_pipe(pipe);
  while (!result.output.empty() &&
         (result.output.back() == '\n' || result.output.back() == '\r')) {
    result.output.pop_back();
  }
  return result;
}

bool command_exists(const char *name) {
#ifdef _WIN32
  const std::string command = "where " + std::string(name) + " >NUL 2>NUL";
#else
  const std::string command =
      "command -v " + std::string(name) + " >/dev/null 2>&1";
#endif
  return std::system(command.c_str()) == 0;
}

std::filesystem::path output_path(const std::string &output) {
#ifdef _WIN32
  return std::filesystem::u8path(output);
#else
  return std::filesystem::path(output);
#endif
}

NativePickResult validate_selection(const std::filesystem::path &path,
                                    NativePickMode mode) {
  std::error_code ec;
  const bool valid = mode == NativePickMode::Directory
                         ? std::filesystem::is_directory(path, ec)
                         : std::filesystem::is_regular_file(path, ec);
  if (ec || !valid) {
    return {.status = NativePickStatus::Error,
            .error = mode == NativePickMode::Directory
                         ? "picker did not return an existing directory"
                         : "picker did not return an existing file"};
  }
  return {.status = NativePickStatus::Selected, .path = path};
}

NativePickResult test_override(NativePickMode mode) {
  const char *key = mode == NativePickMode::Directory
                        ? "KIKO_TEST_NATIVE_PICK_DIR"
                        : "KIKO_TEST_NATIVE_PICK_FILE";
  const char *value = std::getenv(key);
  if (!value || !*value)
    return {};
  return validate_selection(output_path(value), mode);
}

NativePickResult interpret_command(CommandResult result, NativePickMode mode) {
  if (!result.started)
    return {.status = NativePickStatus::Error,
            .error = "failed to start system picker"};
  if (result.status != 0) {
    if (result.output.empty() ||
        result.output.find("-128") != std::string::npos ||
        result.output.find("canceled") != std::string::npos ||
        result.output.find("cancelled") != std::string::npos) {
      return {.status = NativePickStatus::Canceled};
    }
    return {.status = NativePickStatus::Error,
            .error = "system picker failed: " + result.output};
  }
  if (result.output.empty())
    return {.status = NativePickStatus::Canceled};
  return validate_selection(output_path(result.output), mode);
}

} // namespace

NativePickResult pick_native_path(NativePickMode mode) {
  auto override = test_override(mode);
  if (override.status != NativePickStatus::Unavailable)
    return override;

#ifdef __APPLE__
  if (!command_exists("osascript")) {
    return {.status = NativePickStatus::Unavailable,
            .error = "osascript is unavailable"};
  }
  const std::string command =
      mode == NativePickMode::Directory
          ? "osascript -e 'POSIX path of (choose folder with prompt \"Choose a "
            "folder for kiko\")' 2>&1"
          : "osascript -e 'POSIX path of (choose file with prompt \"Choose a "
            "file for kiko\")' 2>&1";
  return interpret_command(run_command(command), mode);
#elif defined(_WIN32)
  if (!command_exists("powershell")) {
    return {.status = NativePickStatus::Unavailable,
            .error = "PowerShell is unavailable"};
  }
  const std::string command =
      mode == NativePickMode::Directory
          ? "powershell -NoProfile -STA -Command \"Add-Type -AssemblyName "
            "System.Windows.Forms; "
            "$d=New-Object System.Windows.Forms.FolderBrowserDialog; "
            "if($d.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK){"
            "[Console]::OutputEncoding=[System.Text.Encoding]::UTF8; "
            "[Console]::Out.Write($d.SelectedPath)}\" 2>&1"
          : "powershell -NoProfile -STA -Command \"Add-Type -AssemblyName "
            "System.Windows.Forms; "
            "$d=New-Object System.Windows.Forms.OpenFileDialog; "
            "if($d.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK){"
            "[Console]::OutputEncoding=[System.Text.Encoding]::UTF8; "
            "[Console]::Out.Write($d.FileName)}\" 2>&1";
  return interpret_command(run_command(command), mode);
#else
  if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY")) {
    return {.status = NativePickStatus::Unavailable,
            .error = "no desktop display is available"};
  }
  if (command_exists("zenity")) {
    const std::string command =
        mode == NativePickMode::Directory
            ? "zenity --file-selection --directory --title='Choose a folder "
              "for kiko' 2>&1"
            : "zenity --file-selection --title='Choose a file for kiko' 2>&1";
    return interpret_command(run_command(command), mode);
  }
  if (command_exists("kdialog")) {
    const std::string command =
        mode == NativePickMode::Directory
            ? "kdialog --getexistingdirectory . 'Choose a folder for kiko' 2>&1"
            : "kdialog --getopenfilename . 'All files (*)' 2>&1";
    return interpret_command(run_command(command), mode);
  }
  return {.status = NativePickStatus::Unavailable,
          .error = "install zenity or kdialog to use the system picker"};
#endif
}

} // namespace kiko
