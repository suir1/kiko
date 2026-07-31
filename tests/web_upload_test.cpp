#include "web/web_upload.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
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
  assert(id->size() == 32);
  assert(std::all_of(id->begin(), id->end(), [](const char value) {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
  }));

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

  {
    WebUploadLimits limits;
    limits.max_active_uploads = 2;
    limits.free_space_reserve_bytes = 0;
    WebUploadStore limited(limits);
    const auto first = limited.start("first.txt", 0, error);
    const auto second = limited.start("second.txt", 0, error);
    assert(first && second);
    error.clear();
    assert(!limited.start("third.txt", 0, error));
    assert(error == "too many active browser uploads");
    limited.cancel(*first);
    error.clear();
    assert(limited.start("third.txt", 0, error));
  }

  {
    WebUploadLimits limits;
    limits.free_space_reserve_bytes = 0;
    WebUploadStore reserved(limits);
    std::error_code space_error;
    const auto space = std::filesystem::space(std::filesystem::temp_directory_path(), space_error);
    assert(!space_error);
    const auto claim = static_cast<std::uint64_t>(space.available - space.available / 4);
    const auto first = reserved.start("large-a.bin", claim, error);
    assert(first);
    error.clear();
    assert(!reserved.start("large-b.bin", claim, error));
    assert(error == "not enough unreserved space to stage selected file");
    reserved.cancel(*first);
  }

  {
    WebUploadLimits limits;
    limits.free_space_reserve_bytes = 0;
    limits.idle_ttl = std::chrono::seconds(1);
    WebUploadStore expiring(limits);
    const auto expiring_id = expiring.start("expired.txt", 3, error);
    assert(expiring_id);
    assert(expiring.append(*expiring_id, 0, "old", error));
    const auto staged_expiring = expiring.finish(*expiring_id, error);
    assert(staged_expiring && std::filesystem::exists(staged_expiring->path));
    expiring.purge_expired(std::chrono::steady_clock::now() + std::chrono::seconds(2));
    error.clear();
    assert(!expiring.completed_path(*expiring_id, error));
    assert(!std::filesystem::exists(staged_expiring->path));
  }

  std::cout << "web upload ok\n";
  return 0;
}
