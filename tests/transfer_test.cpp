#include "file_metadata.hpp"
#include "platform.hpp"
#include "protocol.hpp"
#include "relay_server.hpp"
#include "transfer.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <span>
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

std::string random_blob(std::size_t n) {
  std::mt19937 rng(12345);
  std::uniform_int_distribution<int> dist(0, 255);
  std::string s;
  s.reserve(n);
  for (std::size_t i = 0; i < n; ++i) s.push_back(static_cast<char>(dist(rng)));
  return s;
}

void set_mtime_ms(const fs::path& path, std::uint64_t ms) {
  kiko::detail::apply_mtime_ms(path, ms);
}

void set_mode_bits(const fs::path& path, std::uint32_t mode) {
  kiko::detail::apply_file_mode_bits(path, mode);
}

std::uint64_t get_mtime_ms(const fs::path& path) {
  return kiko::detail::file_mtime_ms(path);
}

std::uint32_t get_mode_bits(const fs::path& path) {
  return kiko::detail::file_mode_bits(path);
}

std::uint64_t abs_diff_u64(std::uint64_t a, std::uint64_t b) {
  return a > b ? a - b : b - a;
}

enum class TestStreamTag : std::uint8_t {
  FileHeader = 1,
  Data = 2,
  FileEnd = 3,
  Done = 4,
  Resume = 5,
  ResumeAck = 8,
  Manifest = 9,
};

struct TestTaggedFrame {
  TestStreamTag tag;
  Bytes payload;
};

void send_test_tagged(TcpSocket& socket, StreamCipher& cipher, TestStreamTag tag,
                      std::span<const std::uint8_t> payload) {
  Bytes plain;
  plain.reserve(payload.size() + 1);
  plain.push_back(static_cast<std::uint8_t>(tag));
  plain.insert(plain.end(), payload.begin(), payload.end());
  send_frame(socket, cipher.encrypt(plain));
}

void send_test_tagged_text(TcpSocket& socket, StreamCipher& cipher, TestStreamTag tag, const std::string& text) {
  send_test_tagged(socket, cipher, tag,
                   std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

std::optional<TestTaggedFrame> recv_test_tagged(TcpSocket& socket, StreamCipher& cipher) {
  auto encrypted = recv_frame(socket);
  if (!encrypted) return std::nullopt;
  auto plain = cipher.decrypt(*encrypted);
  if (plain.empty()) throw KikoError("empty tagged frame");
  TestTaggedFrame frame{static_cast<TestStreamTag>(plain[0]), {}};
  frame.payload.assign(plain.begin() + 1, plain.end());
  return frame;
}

struct DropOnceReporter : ProgressReporter {
  bool dropped = false;
  bool finished = false;
  int retries = 0;
  std::vector<std::string> resumes;
  std::uint64_t advanced_since_retry = 0;

  void file_advance(std::uint64_t delta) override {
    advanced_since_retry += delta;
    if (!dropped && advanced_since_retry >= 64 * 1024) {
      dropped = true;
      throw KikoError("simulated connection interrupted");
    }
  }

  void file_resume(const std::string& path, std::uint64_t offset, std::uint64_t size) override {
    resumes.push_back(path + ":" + std::to_string(offset) + "/" + std::to_string(size));
  }

  void transfer_retry(int, int, const std::string&) override {
    ++retries;
    advanced_since_retry = 0;
  }

  void transfer_complete(std::size_t, std::uint64_t) override { finished = true; }
};

struct StatusReporter : ProgressReporter {
  std::vector<std::string> statuses;

  void status(const std::string& message) override { statuses.push_back(message); }
};

bool saw_status_containing(const StatusReporter& reporter, const std::string& needle) {
  for (const auto& status : reporter.statuses) {
    if (status.find(needle) != std::string::npos) return true;
  }
  return false;
}

bool run_stream_round(TcpListener& listener, const Endpoint& endpoint, const SessionKey& key,
                      const std::vector<FileEntry>& files, const fs::path& dst,
                      ConflictPolicy conflict_policy = ConflictPolicy::Overwrite,
                      ProgressReporter* receiver_reporter = nullptr, ProgressReporter* sender_reporter = nullptr) {
  ProgressReporter default_sender_reporter;
  ProgressReporter default_receiver_reporter;
  auto& send_reporter = sender_reporter ? *sender_reporter : default_sender_reporter;
  auto& recv_reporter = receiver_reporter ? *receiver_reporter : default_receiver_reporter;
  bool sender_failed = false;
  std::thread sender([&] {
    try {
      auto socket = connect_tcp(endpoint, std::chrono::seconds(2));
      if (!socket.valid()) throw std::runtime_error("connect failed");
      send_files(socket, key, files, send_reporter);
    } catch (const std::exception& e) {
      std::cerr << "sender(stream round) error: " << e.what() << "\n";
      sender_failed = true;
    }
  });

  bool receiver_failed = false;
  try {
    auto accepted = listener.accept(std::chrono::seconds(2));
    if (!accepted.valid()) throw std::runtime_error("accept failed");
    receive_files(accepted, key, dst, recv_reporter, conflict_policy);
  } catch (const std::exception& e) {
    std::cerr << "receiver(stream round) error: " << e.what() << "\n";
    receiver_failed = true;
  }
  sender.join();
  return !sender_failed && !receiver_failed;
}

}  // namespace

int main() {
  auto root = fs::temp_directory_path() / ("kiko_transfer_test_" + std::to_string(process_id()));
  auto src = root / "src" / "payload";
  auto dst = root / "out";
  fs::remove_all(root);

  // A directory with nested files of varying sizes, including a binary blob.
  write_file(src / "a.txt", "hello world\n");
  write_file(src / "nested" / "b.txt", "second file contents\n");
  std::string blob = random_blob(300 * 1024);
  write_file(src / "nested" / "blob.bin", blob);
  fs::create_directory(src / "empty");

  constexpr std::uint64_t kTestMtime = 1'600'000'000'000ULL;
  set_mtime_ms(src / "a.txt", kTestMtime);
  set_mode_bits(src / "a.txt", 0111);
  const auto source_exec_mode = get_mode_bits(src / "a.txt") & 0111;

  auto files = collect_files(src);
  if (files.size() != 4) {
    std::cerr << "FAIL: expected 4 entries, got " << files.size() << "\n";
    return 1;
  }

  SessionKey key{};
  for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::uint8_t>(i * 7 + 1);

  auto listener = TcpListener::bind(Endpoint{"127.0.0.1", 0});
  auto endpoint = listener.local_endpoint();

  ProgressReporter null_reporter;

  bool sender_failed = false;
  std::thread sender([&] {
    try {
      auto socket = connect_tcp(endpoint, std::chrono::seconds(2));
      if (!socket.valid()) throw std::runtime_error("connect failed");
      send_files(socket, key, files, null_reporter);
    } catch (const std::exception& e) {
      std::cerr << "sender error: " << e.what() << "\n";
      sender_failed = true;
    }
  });

  bool receiver_failed = false;
  try {
    auto accepted = listener.accept(std::chrono::seconds(2));
    if (!accepted.valid()) throw std::runtime_error("accept failed");
    receive_files(accepted, key, dst, null_reporter);
  } catch (const std::exception& e) {
    std::cerr << "receiver error: " << e.what() << "\n";
    receiver_failed = true;
  }
  sender.join();

  if (sender_failed || receiver_failed) {
    std::cerr << "FAIL: transfer raised an error\n";
    return 1;
  }

  struct Check {
    std::string rel;
    std::string expected;
  };
  std::vector<Check> checks{
      {"payload/a.txt", "hello world\n"},
      {"payload/nested/b.txt", "second file contents\n"},
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
  if (!fs::is_directory(dst / "payload/empty")) {
    std::cerr << "FAIL: missing received empty directory payload/empty\n";
    return 1;
  }
  auto received_mtime = get_mtime_ms(dst / "payload/a.txt");
  if (abs_diff_u64(received_mtime, kTestMtime) > 2000) {
    std::cerr << "FAIL: mtime not preserved for payload/a.txt, expected " << kTestMtime << ", got "
              << received_mtime << "\n";
    return 1;
  }
  if (source_exec_mode != 0 && (get_mode_bits(dst / "payload/a.txt") & source_exec_mode) != source_exec_mode) {
    std::cerr << "FAIL: executable bits not preserved for payload/a.txt\n";
    return 1;
  }

  // Resume phase: drop the largest file and stage a correct partial so the
  // receiver should resume from the existing bytes rather than restart.
  {
    auto blob_rel = fs::path("payload/nested/blob.bin");
    auto final_blob = dst / blob_rel;
    auto part_blob = dst;
    part_blob /= blob_rel;
    part_blob += ".kikopart";
    fs::remove(final_blob);
    write_file(part_blob, blob.substr(0, blob.size() / 2));

    bool s_failed = false;
    std::thread sender2([&] {
      try {
        auto socket = connect_tcp(endpoint, std::chrono::seconds(2));
        if (!socket.valid()) throw std::runtime_error("connect failed");
        send_files(socket, key, files, null_reporter);
      } catch (const std::exception& e) {
        std::cerr << "sender(resume) error: " << e.what() << "\n";
        s_failed = true;
      }
    });
    bool r_failed = false;
    try {
      auto accepted = listener.accept(std::chrono::seconds(2));
      if (!accepted.valid()) throw std::runtime_error("accept failed");
      receive_files(accepted, key, dst, null_reporter);
    } catch (const std::exception& e) {
      std::cerr << "receiver(resume) error: " << e.what() << "\n";
      r_failed = true;
    }
    sender2.join();

    if (s_failed || r_failed) {
      std::cerr << "FAIL: resume transfer raised an error\n";
      return 1;
    }
    if (read_file(final_blob) != blob) {
      std::cerr << "FAIL: resumed file content mismatch\n";
      return 1;
    }
    if (fs::exists(part_blob)) {
      std::cerr << "FAIL: partial file not finalized after resume\n";
      return 1;
    }
  }

  // Corrupt partials should be rejected before resuming. The sender verifies
  // the receiver's prefix digest and asks it to restart this file from byte 0.
  {
    auto blob_rel = fs::path("payload/nested/blob.bin");
    auto final_blob = dst / blob_rel;
    auto part_blob = dst;
    part_blob /= blob_rel;
    part_blob += ".kikopart";
    fs::remove(final_blob);
    write_file(part_blob, std::string(blob.size() / 2, 'x'));

    bool s_failed = false;
    std::thread sender3([&] {
      try {
        auto socket = connect_tcp(endpoint, std::chrono::seconds(2));
        if (!socket.valid()) throw std::runtime_error("connect failed");
        send_files(socket, key, files, null_reporter);
      } catch (const std::exception& e) {
        std::cerr << "sender(corrupt resume) error: " << e.what() << "\n";
        s_failed = true;
      }
    });
    bool r_failed = false;
    try {
      auto accepted = listener.accept(std::chrono::seconds(2));
      if (!accepted.valid()) throw std::runtime_error("accept failed");
      receive_files(accepted, key, dst, null_reporter);
    } catch (const std::exception& e) {
      std::cerr << "receiver(corrupt resume) error: " << e.what() << "\n";
      r_failed = true;
    }
    sender3.join();

    if (s_failed || r_failed) {
      std::cerr << "FAIL: corrupt partial restart raised an error\n";
      return 1;
    }
    if (read_file(final_blob) != blob) {
      std::cerr << "FAIL: corrupt partial was not restarted correctly\n";
      return 1;
    }
    if (fs::exists(part_blob)) {
      std::cerr << "FAIL: corrupt partial not finalized after restart\n";
      return 1;
    }
  }

  {
    StatusReporter sender_skip_reporter;
    if (!run_stream_round(listener, endpoint, key, files, dst, ConflictPolicy::Overwrite, nullptr,
                          &sender_skip_reporter)) {
      std::cerr << "FAIL: duplicate fast-skip transfer raised an error\n";
      return 1;
    }
    if (!saw_status_containing(sender_skip_reporter, "skipped already-complete payload/nested/blob.bin")) {
      std::cerr << "FAIL: sender did not fast-skip completed duplicate file\n";
      return 1;
    }
  }

  {
    auto conflict_src = root / "conflict-src" / "payload";
    auto conflict_dst = root / "conflict-out";
    write_file(conflict_src / "same.txt", "new contents\n");
    auto conflict_files = collect_files(conflict_src);

    write_file(conflict_dst / "payload/same.txt", "keep me\n");
    StatusReporter skip_reporter;
    if (!run_stream_round(listener, endpoint, key, conflict_files, conflict_dst, ConflictPolicy::Skip, &skip_reporter)) {
      std::cerr << "FAIL: skip conflict transfer raised an error\n";
      return 1;
    }
    if (read_file(conflict_dst / "payload/same.txt") != "keep me\n") {
      std::cerr << "FAIL: skip conflict overwrote existing file\n";
      return 1;
    }
    if (!saw_status_containing(skip_reporter, "receive plan:") || !saw_status_containing(skip_reporter, "skip=1")) {
      std::cerr << "FAIL: skip conflict was not included in receive plan\n";
      return 1;
    }

    fs::remove_all(conflict_dst);
    write_file(conflict_dst / "payload/same.txt", "keep me\n");
    StatusReporter rename_reporter;
    if (!run_stream_round(listener, endpoint, key, conflict_files, conflict_dst, ConflictPolicy::Rename, &rename_reporter)) {
      std::cerr << "FAIL: rename conflict transfer raised an error\n";
      return 1;
    }
    if (read_file(conflict_dst / "payload/same.txt") != "keep me\n") {
      std::cerr << "FAIL: rename conflict changed original file\n";
      return 1;
    }
    if (read_file(conflict_dst / "payload/same (1).txt") != "new contents\n") {
      std::cerr << "FAIL: rename conflict did not write renamed file\n";
      return 1;
    }
    if (!saw_status_containing(rename_reporter, "receive plan:") || !saw_status_containing(rename_reporter, "rename=1")) {
      std::cerr << "FAIL: rename conflict was not included in receive plan\n";
      return 1;
    }
  }

  // Protocol hardening: a peer must not be able to write more bytes than the
  // size declared in the file header.
  {
    auto attack_dst = root / "oversize-out";
    bool attacker_failed = false;
    std::thread attacker([&] {
      try {
        auto socket = connect_tcp(endpoint, std::chrono::seconds(2));
        if (!socket.valid()) throw std::runtime_error("connect failed");
        StreamCipher cipher(key, /*sender_originates=*/true);

        Message header{"file", {{"path", "evil.bin"}, {"size", "1"}, {"compress", "none"}}};
        send_test_tagged_text(socket, cipher, TestStreamTag::FileHeader, encode_message(header));
        auto resume = recv_test_tagged(socket, cipher);
        if (!resume || resume->tag != TestStreamTag::Resume) throw std::runtime_error("expected resume");
        Message ack{"resume_ack", {{"offset", "0"}}};
        send_test_tagged_text(socket, cipher, TestStreamTag::ResumeAck, encode_message(ack));

        Bytes payload{'a', 'b'};
        send_test_tagged(socket, cipher, TestStreamTag::Data, payload);
      } catch (const std::exception& e) {
        std::cerr << "attacker error: " << e.what() << "\n";
        attacker_failed = true;
      }
    });

    bool rejected_oversize = false;
    try {
      auto accepted = listener.accept(std::chrono::seconds(2));
      if (!accepted.valid()) throw std::runtime_error("accept failed");
      receive_files(accepted, key, attack_dst, null_reporter);
    } catch (const std::exception& e) {
      const std::string error = e.what();
      rejected_oversize = error.find("more data than declared") != std::string::npos;
      if (!rejected_oversize) std::cerr << "oversize receiver error: " << error << "\n";
    }
    attacker.join();

    if (attacker_failed || !rejected_oversize) {
      std::cerr << "FAIL: receiver did not reject oversized data frame\n";
      return 1;
    }
    if (fs::exists(attack_dst / "evil.bin")) {
      std::cerr << "FAIL: oversized data finalized a file\n";
      return 1;
    }
  }

  // Symlinks default to the historical behavior (follow file contents), but
  // explicit preserve mode sends a safe relative symlink as metadata.
  {
    auto link_src = root / "symlink-src" / "payload";
    auto link_dst = root / "symlink-out";
    write_file(link_src / "target.txt", "linked target\n");
    const bool symlink_supported = try_create_symlink("target.txt", link_src / "link.txt");

    if (symlink_supported) {
      auto follow_files = collect_files(link_src);
      bool followed_as_file = false;
      for (const auto& entry : follow_files) {
        if (entry.relative == "payload/link.txt" && !entry.symlink) followed_as_file = true;
      }
      if (!followed_as_file) {
        std::cerr << "FAIL: default symlink mode did not follow link contents\n";
        return 1;
      }

      CollectOptions preserve_opts;
      preserve_opts.symlink_mode = SymlinkMode::Preserve;
      auto preserve_files = collect_files(link_src, preserve_opts);
      bool saw_symlink = false;
      for (const auto& entry : preserve_files) {
        if (entry.relative == "payload/link.txt" && entry.symlink && entry.link_target == "target.txt") {
          saw_symlink = true;
        }
      }
      if (!saw_symlink) {
        std::cerr << "FAIL: preserve mode did not collect symlink metadata\n";
        return 1;
      }

      bool s_failed = false;
      std::thread symlink_sender([&] {
        try {
          auto socket = connect_tcp(endpoint, std::chrono::seconds(2));
          if (!socket.valid()) throw std::runtime_error("connect failed");
          send_files(socket, key, preserve_files, null_reporter);
        } catch (const std::exception& e) {
          std::cerr << "sender(symlink preserve) error: " << e.what() << "\n";
          s_failed = true;
        }
      });
      bool r_failed = false;
      try {
        auto accepted = listener.accept(std::chrono::seconds(2));
        if (!accepted.valid()) throw std::runtime_error("accept failed");
        receive_files(accepted, key, link_dst, null_reporter);
      } catch (const std::exception& e) {
        std::cerr << "receiver(symlink preserve) error: " << e.what() << "\n";
        r_failed = true;
      }
      symlink_sender.join();
      if (s_failed || r_failed) {
        std::cerr << "FAIL: symlink preserve transfer raised an error\n";
        return 1;
      }
      const auto received_link = link_dst / "payload/link.txt";
      if (!is_symlink_path(received_link) || fs::read_symlink(received_link).generic_string() != "target.txt") {
        std::cerr << "FAIL: symlink target was not preserved\n";
        return 1;
      }
    }
  }

  // Protocol hardening: preserved symlinks must not point outside the receive
  // directory, even if a non-kiko peer sends a malicious header.
  {
    auto attack_dst = root / "symlink-attack-out";
    bool attacker_failed = false;
    std::thread attacker([&] {
      try {
        auto socket = connect_tcp(endpoint, std::chrono::seconds(2));
        if (!socket.valid()) throw std::runtime_error("connect failed");
        StreamCipher cipher(key, /*sender_originates=*/true);

        Message header{"file",
                       {{"path", "bad-link"},
                        {"size", "0"},
                        {"compress", "none"},
                        {"kind", "symlink"},
                        {"target", "../escape"}}};
        send_test_tagged_text(socket, cipher, TestStreamTag::FileHeader, encode_message(header));
      } catch (const std::exception& e) {
        std::cerr << "symlink attacker error: " << e.what() << "\n";
        attacker_failed = true;
      }
    });

    bool rejected_symlink = false;
    try {
      auto accepted = listener.accept(std::chrono::seconds(2));
      if (!accepted.valid()) throw std::runtime_error("accept failed");
      receive_files(accepted, key, attack_dst, null_reporter);
    } catch (const std::exception& e) {
      const std::string error = e.what();
      rejected_symlink = error.find("unsafe symlink target") != std::string::npos;
      if (!rejected_symlink) std::cerr << "symlink receiver error: " << error << "\n";
    }
    attacker.join();

    if (attacker_failed || !rejected_symlink) {
      std::cerr << "FAIL: receiver did not reject unsafe symlink target\n";
      return 1;
    }
  }

  // Manifest preflight should reject unsafe paths before any file body is
  // exchanged or written.
  {
    auto attack_dst = root / "manifest-attack-out";
    bool attacker_failed = false;
    std::thread attacker([&] {
      try {
        auto socket = connect_tcp(endpoint, std::chrono::seconds(2));
        if (!socket.valid()) throw std::runtime_error("connect failed");
        StreamCipher cipher(key, /*sender_originates=*/true);
        const std::string manifest =
            R"({"version":1,"count":1,"total_size":1,"entries":[{"path":"../evil.bin","kind":"file","size":1}]})";
        send_test_tagged_text(socket, cipher, TestStreamTag::Manifest, manifest);
      } catch (const std::exception& e) {
        std::cerr << "manifest attacker error: " << e.what() << "\n";
        attacker_failed = true;
      }
    });

    bool rejected_manifest_path = false;
    try {
      auto accepted = listener.accept(std::chrono::seconds(2));
      if (!accepted.valid()) throw std::runtime_error("accept failed");
      receive_files(accepted, key, attack_dst, null_reporter);
    } catch (const std::exception& e) {
      const std::string error = e.what();
      rejected_manifest_path = error.find("path traversal") != std::string::npos;
      if (!rejected_manifest_path) std::cerr << "manifest receiver error: " << error << "\n";
    }
    attacker.join();

    if (attacker_failed || !rejected_manifest_path) {
      std::cerr << "FAIL: receiver did not reject unsafe manifest path\n";
      return 1;
    }
    if (fs::exists(root / "evil.bin") || fs::exists(attack_dst / "evil.bin")) {
      std::cerr << "FAIL: unsafe manifest path created a file\n";
      return 1;
    }
  }

  {
    auto reconnect_src = root / "auto-src" / "payload";
    auto reconnect_dst = root / "auto-out";
    write_file(reconnect_src / "big.bin", random_blob(512 * 1024));

    BackgroundRelay relay;
    relay.start(Endpoint{"127.0.0.1", 0});

    SendConfig send_config;
    send_config.file = reconnect_src;
    send_config.relay = relay.local_endpoint();
    send_config.code = "retry123";
    send_config.no_direct = true;
    send_config.lan_discover = false;
    send_config.disable_local = true;
    send_config.show_qrcode = false;
    send_config.connections = 1;
    send_config.reconnect_attempts = 3;
    send_config.reconnect_delay = std::chrono::milliseconds(100);

    RecvConfig recv_config;
    recv_config.code = send_config.code;
    recv_config.relay = relay.local_endpoint();
    recv_config.output_dir = reconnect_dst;
    recv_config.no_direct = true;
    recv_config.lan_discover = false;
    recv_config.disable_local = true;
    recv_config.reconnect_attempts = 3;
    recv_config.reconnect_delay = std::chrono::milliseconds(100);

    ProgressReporter sender_reporter;
    DropOnceReporter receiver_reporter;
    std::exception_ptr sender_error;
    std::exception_ptr receiver_error;

    std::thread sender([&] {
      try {
        run_send(send_config, sender_reporter);
      } catch (...) {
        sender_error = std::current_exception();
      }
    });
    std::thread receiver([&] {
      try {
        run_recv(recv_config, receiver_reporter);
      } catch (...) {
        receiver_error = std::current_exception();
      }
    });

    sender.join();
    receiver.join();
    relay.stop();

    if (sender_error || receiver_error) {
      try {
        if (sender_error) std::rethrow_exception(sender_error);
        if (receiver_error) std::rethrow_exception(receiver_error);
      } catch (const std::exception& e) {
        std::cerr << "FAIL: auto reconnect transfer raised: " << e.what() << "\n";
      }
      return 1;
    }
    if (!receiver_reporter.dropped || receiver_reporter.retries < 1 || !receiver_reporter.finished) {
      std::cerr << "FAIL: auto reconnect did not retry and finish\n";
      return 1;
    }
    if (read_file(reconnect_dst / "payload/big.bin") != read_file(reconnect_src / "big.bin")) {
      std::cerr << "FAIL: auto reconnect output mismatch\n";
      return 1;
    }
  }

  {
    auto reconnect_src = root / "mux-auto-src" / "payload";
    auto reconnect_dst = root / "mux-auto-out";
    write_file(reconnect_src / "big.bin", random_blob(768 * 1024));

    BackgroundRelay relay;
    relay.start(Endpoint{"127.0.0.1", 0});

    SendConfig send_config;
    send_config.file = reconnect_src;
    send_config.relay = relay.local_endpoint();
    send_config.code = "muxretry123";
    send_config.no_direct = true;
    send_config.lan_discover = false;
    send_config.disable_local = true;
    send_config.show_qrcode = false;
    send_config.connections = 4;
    send_config.reconnect_attempts = 3;
    send_config.reconnect_delay = std::chrono::milliseconds(100);

    RecvConfig recv_config;
    recv_config.code = send_config.code;
    recv_config.relay = relay.local_endpoint();
    recv_config.output_dir = reconnect_dst;
    recv_config.no_direct = true;
    recv_config.lan_discover = false;
    recv_config.disable_local = true;
    recv_config.reconnect_attempts = 3;
    recv_config.reconnect_delay = std::chrono::milliseconds(100);

    ProgressReporter sender_reporter;
    DropOnceReporter receiver_reporter;
    std::exception_ptr sender_error;
    std::exception_ptr receiver_error;

    std::thread sender([&] {
      try {
        run_send(send_config, sender_reporter);
      } catch (...) {
        sender_error = std::current_exception();
      }
    });
    std::thread receiver([&] {
      try {
        run_recv(recv_config, receiver_reporter);
      } catch (...) {
        receiver_error = std::current_exception();
      }
    });

    sender.join();
    receiver.join();
    relay.stop();

    if (sender_error || receiver_error) {
      try {
        if (sender_error) std::rethrow_exception(sender_error);
        if (receiver_error) std::rethrow_exception(receiver_error);
      } catch (const std::exception& e) {
        std::cerr << "FAIL: mux auto reconnect transfer raised: " << e.what() << "\n";
      }
      return 1;
    }
    if (!receiver_reporter.dropped || receiver_reporter.retries < 1 || !receiver_reporter.finished) {
      std::cerr << "FAIL: mux auto reconnect did not retry and finish\n";
      return 1;
    }
    if (receiver_reporter.resumes.empty()) {
      std::cerr << "FAIL: mux auto reconnect did not resume partial data\n";
      return 1;
    }
    if (read_file(reconnect_dst / "payload/big.bin") != read_file(reconnect_src / "big.bin")) {
      std::cerr << "FAIL: mux auto reconnect output mismatch\n";
      return 1;
    }
    if (fs::exists(reconnect_dst / "payload/big.bin.kikopart")) {
      std::cerr << "FAIL: mux auto reconnect left partial file behind\n";
      return 1;
    }
  }

  fs::remove_all(root);
  std::cout << "PASS: multi-file directory round-trip + resume with integrity verification\n";
  return 0;
}
