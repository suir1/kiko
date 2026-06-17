#pragma once

#include "common.hpp"

#include <sodium.h>

#include <array>
#include <span>

namespace kiko {

using SessionKey = std::array<std::uint8_t, 32>;

// XChaCha20-Poly1305 session cipher. Each message carries a random 24-byte nonce.
// Per-direction and per-stream subkeys prevent nonce reuse across peers and mux channels.
class StreamCipher {
 public:
  explicit StreamCipher(const SessionKey& key, bool sender_originates = true, std::uint8_t stream_id = 0);

  [[nodiscard]] Bytes encrypt(std::span<const std::uint8_t> plaintext);
  [[nodiscard]] Bytes decrypt(std::span<const std::uint8_t> ciphertext);

 private:
  std::array<std::uint8_t, 32> send_key_{};
  std::array<std::uint8_t, 32> recv_key_{};
};

[[nodiscard]] Bytes sha256(std::span<const std::uint8_t> input);

class Sha256Hasher {
 public:
  Sha256Hasher();
  Sha256Hasher(const Sha256Hasher&) = delete;
  Sha256Hasher& operator=(const Sha256Hasher&) = delete;
  ~Sha256Hasher();

  void update(std::span<const std::uint8_t> input);
  [[nodiscard]] Bytes finish();

 private:
  crypto_hash_sha256_state state_{};
  bool initialized_ = false;
};

[[nodiscard]] Bytes hmac_sha256(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data);
[[nodiscard]] Bytes hkdf_sha256(std::span<const std::uint8_t> ikm, std::span<const std::uint8_t> salt,
                                std::span<const std::uint8_t> info, std::size_t length);
[[nodiscard]] bool constant_time_equal(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b);

}  // namespace kiko
