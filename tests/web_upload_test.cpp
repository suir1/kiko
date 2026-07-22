#include "web/web_upload.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

int main() {
  using namespace kiko;

  WebUploadStore uploads;
  std::string error;
  const std::string content(700 * 1024, 'k');
  const auto id = uploads.start("../picked.txt", content.size(), error);
  assert(id);

  assert(uploads.append(*id, 0, std::string_view(content).substr(0, 512 * 1024),
                        error));
  error.clear();
  assert(!uploads.append(*id, 1, "bad offset", error));
  assert(error == "upload chunk offset mismatch");
  error.clear();
  assert(uploads.append(*id, 512 * 1024,
                        std::string_view(content).substr(512 * 1024), error));

  const auto completed = uploads.finish(*id, error);
  assert(completed);
  assert(completed->path.filename() == "picked.txt");
  std::ifstream file(completed->path, std::ios::binary);
  const std::string staged((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
  assert(staged == content);
  assert(uploads.completed_path(*id, error) == completed->path);

  uploads.release(*id);
  assert(std::filesystem::is_regular_file(completed->path));

  const auto canceled_id = uploads.start("cancel.txt", 3, error);
  assert(canceled_id);
  assert(uploads.append(*canceled_id, 0, "abc", error));
  const auto canceled = uploads.finish(*canceled_id, error);
  assert(canceled);
  uploads.cancel(*canceled_id);
  assert(!std::filesystem::exists(canceled->path));

  std::cout << "web upload ok\n";
  return 0;
}
