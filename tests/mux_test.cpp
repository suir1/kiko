#include "transfer/file_metadata.hpp"
#include "platform.hpp"
#include "transfer/transfer.hpp"
#include "transfer/transfer_stream.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace kiko;

namespace {

void write_file(const fs::path& path, const std::string& contents) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

std::string read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool try_create_symlink(const fs::path& target, const fs::path& link_path) {
  std::error_code ec;
  fs::create_directories(link_path.parent_path());
  fs::create_symlink(target, link_path, ec);
  return !ec;
}

bool is_symlink_path(const fs::path& path) {
  std::error_code ec;
  return fs::is_symlink(fs::symlink_status(path, ec));
}

std::string random_blob(std::size_t n, unsigned seed) {
  std::mt19937 rng(seed);
  std::uniform_int_distribution<int> dist(0, 255);
  std::string s;
  s.reserve(n);
  for (std::size_t i = 0; i < n; ++i) s.push_back(static_cast<char>(dist(rng)));
  return s;
}

void set_mode_bits(const fs::path& path, std::uint32_t mode) {
  kiko::detail::apply_file_mode_bits(path, mode);
}

std::uint32_t get_mode_bits(const fs::path& path) {
  return kiko::detail::file_mode_bits(path);
}

struct RecordingReporter : ProgressReporter {
  std::vector<std::string> statuses;
  std::vector<std::string> resumes;
  std::vector<TransferTiming> timings;

  void status(const std::string& message) override { statuses.push_back(message); }
  void transfer_timing(const TransferTiming& timing) override { timings.push_back(timing); }
  void file_resume(const std::string& path, std::uint64_t offset, std::uint64_t size) override {
    resumes.push_back(path + ":" + std::to_string(offset) + "/" + std::to_string(size));
  }
};

// Builds N paired, index-ordered loopback connections.
void make_channels(TcpListener& listener, const Endpoint& endpoint, int n,
                   std::vector<TcpSocket>& sender, std::vector<TcpSocket>& receiver) {
  sender.clear();
  receiver.clear();
  sender.resize(static_cast<std::size_t>(n));
  receiver.resize(static_cast<std::size_t>(n));
  std::thread connector([&] {
    for (int k = 0; k < n; ++k) {
      auto s = connect_tcp(endpoint, std::chrono::seconds(2));
      if (!s.valid()) throw std::runtime_error("connect failed");
      auto idx = static_cast<std::uint8_t>(k);
      s.send_all(&idx, 1);
      sender[static_cast<std::size_t>(k)] = std::move(s);
    }
  });
  for (int i = 0; i < n; ++i) {
    auto a = listener.accept(std::chrono::seconds(2));
    if (!a.valid()) throw std::runtime_error("accept failed");
    std::uint8_t idx = 0;
    if (!a.recv_exact(&idx, 1)) throw std::runtime_error("index read failed");
    receiver[idx] = std::move(a);
  }
  connector.join();
}

bool run_round(TcpListener& listener, const Endpoint& endpoint, int n, const SessionKey& key,
               const std::vector<FileEntry>& files, const fs::path& dst, ProgressReporter* receiver_reporter = nullptr,
               ConflictPolicy conflict_policy = ConflictPolicy::Overwrite, ProgressReporter* sender_reporter = nullptr) {
  std::vector<TcpSocket> sc, rc;
  make_channels(listener, endpoint, n, sc, rc);
  ProgressReporter default_sender_reporter;
  ProgressReporter default_receiver_reporter;
  auto& send_reporter = sender_reporter ? *sender_reporter : default_sender_reporter;
  auto& recv_reporter = receiver_reporter ? *receiver_reporter : default_receiver_reporter;
  bool sender_failed = false;
  std::thread sender([&] {
    try {
      send_files_mux(sc, key, files, send_reporter);
    } catch (const std::exception& e) {
      std::cerr << "sender error: " << e.what() << "\n";
      sender_failed = true;
    }
  });
  bool receiver_failed = false;
  try {
    receive_files_mux(rc, key, dst, recv_reporter, conflict_policy);
  } catch (const std::exception& e) {
    std::cerr << "receiver error: " << e.what() << "\n";
    receiver_failed = true;
  }
  sender.join();
  return !sender_failed && !receiver_failed;
}

}  // namespace

int main() {
  auto root = fs::temp_directory_path() / ("kiko_mux_test_" + std::to_string(process_id()));
  auto src = root / "src" / "payload";
  auto dst = root / "out";
  fs::remove_all(root);

  write_file(src / "small.txt", "tiny\n");
  set_mode_bits(src / "small.txt", 0111);
  const auto source_exec_mode = get_mode_bits(src / "small.txt") & 0111;
  write_file(src / "empty.bin", "");
  std::string blob = random_blob(2 * 1024 * 1024 + 1234, 99);  // spans many chunks/channels
  write_file(src / "nested" / "blob.bin", blob);

  auto files = collect_files(src);

  SessionKey key{};
  for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::uint8_t>(i * 5 + 3);

  auto listener = TcpListener::bind(Endpoint{"127.0.0.1", 0});
  auto endpoint = listener.local_endpoint();
  const int N = 4;
  const auto max_expected_mux_pending =
      static_cast<std::uint64_t>(N) * 4 * static_cast<std::uint64_t>(kiko::detail::kMuxChunk);

  RecordingReporter first_sender_reporter;
  RecordingReporter first_receiver_reporter;
  if (!run_round(listener, endpoint, N, key, files, dst, &first_receiver_reporter, ConflictPolicy::Overwrite,
                 &first_sender_reporter)) {
    std::cerr << "FAIL: mux transfer raised an error\n";
    return 1;
  }
  if (first_sender_reporter.timings.size() != 1 || first_sender_reporter.timings[0].mode != "mux_send" ||
      first_sender_reporter.timings[0].mux_channels != static_cast<std::size_t>(N) ||
      first_sender_reporter.timings[0].mux_max_pending_bytes == 0 ||
      first_sender_reporter.timings[0].mux_max_pending_bytes > max_expected_mux_pending ||
      first_sender_reporter.timings[0].payload_bytes != static_cast<std::uint64_t>(blob.size() + 5)) {
    std::cerr << "FAIL: mux sender timing missing or incomplete\n";
    return 1;
  }
  if (first_receiver_reporter.timings.size() != 1 || first_receiver_reporter.timings[0].mode != "mux_receive" ||
      first_receiver_reporter.timings[0].mux_channels != static_cast<std::size_t>(N) ||
      first_receiver_reporter.timings[0].payload_bytes != static_cast<std::uint64_t>(blob.size() + 5)) {
    std::cerr << "FAIL: mux receiver timing missing or incomplete\n";
    return 1;
  }

  struct Check {
    std::string rel;
    std::string expected;
  };
  std::vector<Check> checks{
      {"payload/small.txt", "tiny\n"},
      {"payload/empty.bin", ""},
      {"payload/nested/blob.bin", blob},
  };
  for (const auto& c : checks) {
    auto path = dst / c.rel;
    if (!fs::exists(path)) {
      std::cerr << "FAIL: missing received file " << c.rel << "\n";
      return 1;
    }
    if (read_file(path) != c.expected) {
      std::cerr << "FAIL: content mismatch for " << c.rel << "\n";
      return 1;
    }
  }
  if (source_exec_mode != 0 && (get_mode_bits(dst / "payload/small.txt") & source_exec_mode) != source_exec_mode) {
    std::cerr << "FAIL: executable bits not preserved on mux transfer\n";
    return 1;
  }

  // Resume across the multiplexed path: stage a correct partial for the blob.
  {
    auto blob_rel = fs::path("payload/nested/blob.bin");
    auto final_blob = dst / blob_rel;
    auto part_blob = dst;
    part_blob /= blob_rel;
    part_blob += ".kikopart";
    fs::remove(final_blob);
    write_file(part_blob, blob.substr(0, blob.size() / 3));

    RecordingReporter reporter;
    if (!run_round(listener, endpoint, N, key, files, dst, &reporter)) {
      std::cerr << "FAIL: mux resume transfer raised an error\n";
      return 1;
    }
    const auto expected_resume = "payload/nested/blob.bin:" + std::to_string(blob.size() / 3) + "/" +
                                 std::to_string(blob.size());
    if (reporter.resumes.size() != 1 || reporter.resumes[0] != expected_resume) {
      std::cerr << "FAIL: mux resume event mismatch\n";
      return 1;
    }
    if (read_file(final_blob) != blob) {
      std::cerr << "FAIL: resumed mux file content mismatch\n";
      return 1;
    }
    if (fs::exists(part_blob)) {
      std::cerr << "FAIL: partial not finalized after mux resume\n";
      return 1;
    }
  }

  // Corrupt partials should be rejected before resuming on the mux path too.
  {
    auto blob_rel = fs::path("payload/nested/blob.bin");
    auto final_blob = dst / blob_rel;
    auto part_blob = dst;
    part_blob /= blob_rel;
    part_blob += ".kikopart";
    fs::remove(final_blob);
    write_file(part_blob, std::string(blob.size() / 3, 'x'));

    if (!run_round(listener, endpoint, N, key, files, dst)) {
      std::cerr << "FAIL: mux corrupt partial restart raised an error\n";
      return 1;
    }
    if (read_file(final_blob) != blob) {
      std::cerr << "FAIL: mux corrupt partial was not restarted correctly\n";
      return 1;
    }
    if (fs::exists(part_blob)) {
      std::cerr << "FAIL: mux corrupt partial not finalized after restart\n";
      return 1;
    }
  }

  // Existing complete files should be skipped on the mux path, matching the
  // single-channel imohash fast path.
  {
    RecordingReporter reporter;
    if (!run_round(listener, endpoint, N, key, files, dst, &reporter)) {
      std::cerr << "FAIL: mux duplicate-skip transfer raised an error\n";
      return 1;
    }
    bool saw_blob_skip = false;
    for (const auto& status : reporter.statuses) {
      if (status == "skipped duplicate payload/nested/blob.bin") saw_blob_skip = true;
    }
    if (!saw_blob_skip) {
      std::cerr << "FAIL: mux duplicate file was not skipped\n";
      return 1;
    }
  }

  {
    auto conflict_src = root / "mux-conflict-src" / "payload";
    write_file(conflict_src / "same.txt", "mux new contents\n");
    auto conflict_files = collect_files(conflict_src);

    auto skip_dst = root / "mux-conflict-skip-out";
    write_file(skip_dst / "payload/same.txt", "mux keep me\n");
    if (!run_round(listener, endpoint, N, key, conflict_files, skip_dst, nullptr, ConflictPolicy::Skip)) {
      std::cerr << "FAIL: mux skip conflict transfer raised an error\n";
      return 1;
    }
    if (read_file(skip_dst / "payload/same.txt") != "mux keep me\n") {
      std::cerr << "FAIL: mux skip conflict overwrote existing file\n";
      return 1;
    }

    auto rename_dst = root / "mux-conflict-rename-out";
    write_file(rename_dst / "payload/same.txt", "mux keep me\n");
    if (!run_round(listener, endpoint, N, key, conflict_files, rename_dst, nullptr, ConflictPolicy::Rename)) {
      std::cerr << "FAIL: mux rename conflict transfer raised an error\n";
      return 1;
    }
    if (read_file(rename_dst / "payload/same.txt") != "mux keep me\n") {
      std::cerr << "FAIL: mux rename conflict changed original file\n";
      return 1;
    }
    if (read_file(rename_dst / "payload/same (1).txt") != "mux new contents\n") {
      std::cerr << "FAIL: mux rename conflict did not write renamed file\n";
      return 1;
    }
  }

  {
    auto link_src = root / "mux-symlink-src" / "payload";
    auto link_dst = root / "mux-symlink-out";
    write_file(link_src / "target.txt", "mux linked target\n");
    const bool symlink_supported = try_create_symlink("target.txt", link_src / "link.txt");
    if (symlink_supported) {
      CollectOptions preserve_opts;
      preserve_opts.symlink_mode = SymlinkMode::Preserve;
      auto symlink_files = collect_files(link_src, preserve_opts);
      if (!run_round(listener, endpoint, N, key, symlink_files, link_dst)) {
        std::cerr << "FAIL: mux symlink preserve transfer raised an error\n";
        return 1;
      }
      const auto received_link = link_dst / "payload/link.txt";
      if (!is_symlink_path(received_link) || fs::read_symlink(received_link).generic_string() != "target.txt") {
        std::cerr << "FAIL: mux symlink target was not preserved\n";
        return 1;
      }
    }
  }

  fs::remove_all(root);
  std::cout << "PASS: multiplexed multi-connection transfer + resume\n";
  return 0;
}
