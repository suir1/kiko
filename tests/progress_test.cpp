#include "core/common.hpp"
#include "core/progress.hpp"
#include "transfer/transfer.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
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

// Records the sequence of reporter events for assertions.
struct RecordingReporter : ProgressReporter {
  std::vector<std::string> statuses;
  std::vector<std::string> started;
  std::vector<std::string> resumes;
  std::vector<std::string> completed;
  std::uint64_t advanced = 0;
  std::size_t done_files = 0;
  std::uint64_t done_bytes = 0;
  bool finished = false;
  bool all_verified = true;
  std::vector<TransferTiming> timings;

  void status(const std::string& message) override { statuses.push_back(message); }
  void transfer_timing(const TransferTiming& timing) override { timings.push_back(timing); }
  void file_start(const std::string& path, std::uint64_t) override { started.push_back(path); }
  void file_advance(std::uint64_t delta) override { advanced += delta; }
  void file_resume(const std::string& path, std::uint64_t offset, std::uint64_t size) override {
    resumes.push_back(path + ":" + std::to_string(offset) + "/" + std::to_string(size));
  }
  void file_complete(const std::string& path, std::uint64_t, bool verified) override {
    completed.push_back(path);
    if (!verified) all_verified = false;
  }
  void transfer_complete(std::size_t files, std::uint64_t bytes) override {
    finished = true;
    done_files = files;
    done_bytes = bytes;
  }
};

}  // namespace

int main() {
  auto root = fs::temp_directory_path() / ("kiko_progress_test_" + std::to_string(now_ms()));
  auto src = root / "data";
  auto dst = root / "out";
  fs::remove_all(root);

  write_file(src / "one.txt", std::string(1000, 'a'));
  write_file(src / "two.txt", std::string(2500, 'b'));

  auto files = collect_files(src);
  const std::uint64_t expected_bytes = 1000 + 2500;

  SessionKey key{};
  for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::uint8_t>(i + 1);

  auto listener = TcpListener::bind(Endpoint{"127.0.0.1", 0});
  auto endpoint = listener.local_endpoint();

  RecordingReporter sender_rec;
  std::thread sender([&] {
    auto socket = connect_tcp(endpoint, std::chrono::seconds(2));
    if (socket.valid()) send_files(socket, key, files, sender_rec);
  });

  RecordingReporter rec;
  auto accepted = listener.accept(std::chrono::seconds(2));
  receive_files(accepted, key, dst, rec);
  sender.join();

  if (rec.started.size() != 2) {
    std::cerr << "FAIL: expected 2 file_start, got " << rec.started.size() << "\n";
    return 1;
  }
  if (rec.completed.size() != 2 || !rec.all_verified) {
    std::cerr << "FAIL: expected 2 verified file_complete, got " << rec.completed.size()
              << " verified=" << rec.all_verified << "\n";
    return 1;
  }
  if (!rec.finished || rec.done_files != 2 || rec.done_bytes != expected_bytes) {
    std::cerr << "FAIL: transfer_complete files=" << rec.done_files << " bytes=" << rec.done_bytes
              << " (expected 2 / " << expected_bytes << ")\n";
    return 1;
  }
  if (rec.advanced != expected_bytes) {
    std::cerr << "FAIL: file_advance sum=" << rec.advanced << " (expected " << expected_bytes << ")\n";
    return 1;
  }
  if (sender_rec.timings.size() != 1 || sender_rec.timings[0].mode != "stream_send" ||
      sender_rec.timings[0].payload_bytes != expected_bytes || sender_rec.timings[0].frame_count == 0) {
    std::cerr << "FAIL: sender transfer timing missing or incomplete\n";
    return 1;
  }
  if (rec.timings.size() != 1 || rec.timings[0].mode != "stream_receive" ||
      rec.timings[0].payload_bytes != expected_bytes) {
    std::cerr << "FAIL: receiver transfer timing missing or incomplete\n";
    return 1;
  }

  auto resume_dst = root / "resume-out";
  constexpr std::uint64_t kResumeOffset = 300;
  write_file(resume_dst / "data/one.txt.kikopart", std::string(kResumeOffset, 'a'));

  auto resume_listener = TcpListener::bind(Endpoint{"127.0.0.1", 0});
  auto resume_endpoint = resume_listener.local_endpoint();

  std::thread resume_sender([&] {
    auto socket = connect_tcp(resume_endpoint, std::chrono::seconds(2));
    if (socket.valid()) send_files(socket, key, files, sender_rec);
  });

  RecordingReporter resume_rec;
  auto resume_accepted = resume_listener.accept(std::chrono::seconds(2));
  receive_files(resume_accepted, key, resume_dst, resume_rec);
  resume_sender.join();

  const auto expected_resume = std::string("data/one.txt:") + std::to_string(kResumeOffset) + "/1000";
  if (resume_rec.resumes.size() != 1 || resume_rec.resumes[0] != expected_resume) {
    std::cerr << "FAIL: expected resume event '" << expected_resume << "', got "
              << (resume_rec.resumes.empty() ? std::string("<none>") : resume_rec.resumes[0]) << "\n";
    return 1;
  }
  if (resume_rec.advanced != expected_bytes) {
    std::cerr << "FAIL: resumed file_advance sum=" << resume_rec.advanced << " (expected " << expected_bytes << ")\n";
    return 1;
  }

  auto duplicate_listener = TcpListener::bind(Endpoint{"127.0.0.1", 0});
  auto duplicate_endpoint = duplicate_listener.local_endpoint();

  std::thread duplicate_sender([&] {
    auto socket = connect_tcp(duplicate_endpoint, std::chrono::seconds(2));
    if (socket.valid()) send_files(socket, key, files, sender_rec);
  });

  RecordingReporter duplicate_rec;
  auto duplicate_accepted = duplicate_listener.accept(std::chrono::seconds(2));
  receive_files(duplicate_accepted, key, dst, duplicate_rec);
  duplicate_sender.join();

  if (duplicate_rec.advanced != expected_bytes) {
    std::cerr << "FAIL: duplicate-skip file_advance sum=" << duplicate_rec.advanced << " (expected "
              << expected_bytes << ")\n";
    return 1;
  }

  RecordingReporter retry_rec;
  retry_rec.transfer_retry(2, 3, "connection reset by peer");
  retry_rec.transfer_retry_delay(2, 3, std::chrono::milliseconds(250));
  if (retry_rec.statuses.size() != 2 ||
      retry_rec.statuses[0].find("connection lost, retrying 2/3") == std::string::npos ||
      retry_rec.statuses[0].find("resume will continue verified partial files") == std::string::npos ||
      retry_rec.statuses[0].find("connection reset by peer") == std::string::npos) {
    std::cerr << "FAIL: retry status did not explain reconnect/resume semantics\n";
    return 1;
  }
  if (retry_rec.statuses[1].find("reconnect in 250ms before attempt 2/3") == std::string::npos) {
    std::cerr << "FAIL: retry delay status did not explain reconnect backoff\n";
    return 1;
  }

  RecordingReporter plan_rec;
  ReceivePlanSummary plan_summary;
  plan_summary.item_count = 4;
  plan_summary.total_bytes = 4096;
  plan_summary.resume_count = 1;
  plan_summary.resume_bytes = 1024;
  plan_summary.skip_count = 1;
  plan_summary.skip_bytes = 512;
  plan_summary.rename_count = 1;
  plan_summary.overwrite_count = 1;
  plan_rec.receive_plan(plan_summary);
  if (plan_rec.statuses.size() != 1 ||
      plan_rec.statuses[0].find("receive plan: 4 item(s), 4096 bytes") == std::string::npos ||
      plan_rec.statuses[0].find("skip=1") == std::string::npos ||
      plan_rec.statuses[0].find("rename=1") == std::string::npos ||
      plan_rec.statuses[0].find("overwrite=1") == std::string::npos ||
      plan_rec.statuses[0].find("resume=1 (1024 bytes)") == std::string::npos) {
    std::cerr << "FAIL: receive plan status did not expose preflight actions\n";
    return 1;
  }

  fs::remove_all(root);
  std::cout << "PASS: reporter emits file_start/advance/complete and transfer_complete with correct totals\n";
  return 0;
}
