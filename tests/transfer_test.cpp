#include "file_metadata.hpp"
#include "transfer.hpp"
#include "platform.hpp"
#include "protocol.hpp"

#include <chrono>
#include <cstdint>
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

std::uint64_t get_mtime_ms(const fs::path& path) {
  return kiko::detail::file_mtime_ms(path);
}

enum class TestStreamTag : std::uint8_t {
  FileHeader = 1,
  Data = 2,
  FileEnd = 3,
  Done = 4,
  Resume = 5,
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
  if (get_mtime_ms(dst / "payload/a.txt") != kTestMtime) {
    std::cerr << "FAIL: mtime not preserved for payload/a.txt\n";
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

  fs::remove_all(root);
  std::cout << "PASS: multi-file directory round-trip + resume with integrity verification\n";
  return 0;
}
