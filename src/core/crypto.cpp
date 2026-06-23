#include "core/crypto.hpp"

#include <sodium.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace kiko {
namespace {

struct SodiumInit {
  SodiumInit() {
    if (sodium_init() < 0) throw KikoError("libsodium init failed");
  }
};

const SodiumInit& sodium_ready() {
  static const SodiumInit init;
  return init;
}

SessionKey derive_subkey(const SessionKey& key, const std::string& info) {
  auto derived = hkdf_sha256(key, {}, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(info.data()), info.size()),
                             key.size());
  SessionKey out{};
  std::copy(derived.begin(), derived.end(), out.begin());
  return out;
}

}  // namespace

Bytes sha256(std::span<const std::uint8_t> input) {
  sodium_ready();
  Bytes digest(crypto_hash_sha256_BYTES);
  crypto_hash_sha256(digest.data(), input.data(), input.size());
  return digest;
}

Sha256Hasher::Sha256Hasher() {
  sodium_ready();
  if (crypto_hash_sha256_init(&state_) != 0) throw KikoError("sha256 init failed");
  initialized_ = true;
}

Sha256Hasher::~Sha256Hasher() {
  if (initialized_) sodium_memzero(&state_, sizeof(state_));
}

void Sha256Hasher::update(std::span<const std::uint8_t> input) {
  if (input.empty()) return;
  if (crypto_hash_sha256_update(&state_, input.data(), input.size()) != 0) throw KikoError("sha256 update failed");
}

Bytes Sha256Hasher::finish() {
  Bytes digest(crypto_hash_sha256_BYTES);
  if (crypto_hash_sha256_final(&state_, digest.data()) != 0) throw KikoError("sha256 finalize failed");
  initialized_ = false;
  return digest;
}

Bytes hmac_sha256(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data) {
  sodium_ready();

  constexpr std::size_t block_size = 64;
  constexpr std::size_t digest_size = crypto_hash_sha256_BYTES;

  std::array<std::uint8_t, block_size> key_block{};
  if (key.size() > block_size) {
    auto hashed_key = sha256(key);
    std::copy(hashed_key.begin(), hashed_key.end(), key_block.begin());
  } else if (!key.empty()) {
    std::copy(key.begin(), key.end(), key_block.begin());
  }

  std::array<std::uint8_t, block_size> inner_pad{};
  std::array<std::uint8_t, block_size> outer_pad{};
  for (std::size_t i = 0; i < block_size; ++i) {
    inner_pad[i] = static_cast<std::uint8_t>(key_block[i] ^ 0x36);
    outer_pad[i] = static_cast<std::uint8_t>(key_block[i] ^ 0x5c);
  }

  Sha256Hasher inner;
  inner.update(inner_pad);
  inner.update(data);
  auto inner_digest = inner.finish();

  Sha256Hasher outer;
  outer.update(outer_pad);
  outer.update(inner_digest);
  auto out = outer.finish();

  sodium_memzero(key_block.data(), key_block.size());
  sodium_memzero(inner_pad.data(), inner_pad.size());
  sodium_memzero(outer_pad.data(), outer_pad.size());
  if (out.size() != digest_size) throw KikoError("hmac-sha256 failed");
  return out;
}

Bytes hkdf_sha256(std::span<const std::uint8_t> ikm, std::span<const std::uint8_t> salt,
                  std::span<const std::uint8_t> info, std::size_t length) {
  sodium_ready();
  constexpr std::size_t digest_size = crypto_hash_sha256_BYTES;
  if (length > 255 * digest_size) throw KikoError("hkdf output too long");

  std::array<std::uint8_t, digest_size> empty_salt{};
  auto salt_key = salt.empty() ? std::span<const std::uint8_t>(empty_salt) : salt;
  auto prk = hmac_sha256(salt_key, ikm);

  Bytes out(length);
  Bytes previous;
  std::size_t written = 0;
  for (std::uint8_t counter = 1; written < length; ++counter) {
    Bytes block_input;
    block_input.reserve(previous.size() + info.size() + 1);
    block_input.insert(block_input.end(), previous.begin(), previous.end());
    block_input.insert(block_input.end(), info.begin(), info.end());
    block_input.push_back(counter);

    previous = hmac_sha256(prk, block_input);
    const auto take = std::min(previous.size(), length - written);
    std::copy_n(previous.begin(), take, out.begin() + static_cast<std::ptrdiff_t>(written));
    written += take;
  }
  sodium_memzero(prk.data(), prk.size());
  sodium_memzero(previous.data(), previous.size());
  return out;
}

bool constant_time_equal(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b) {
  if (a.size() != b.size()) return false;
  return sodium_memcmp(a.data(), b.data(), a.size()) == 0;
}

StreamCipher::StreamCipher(const SessionKey& key, bool sender_originates, std::uint8_t stream_id) {
  sodium_ready();
  const std::string stream_tag = std::string("kiko-stream:") + std::to_string(stream_id);
  if (sender_originates) {
    send_key_ = derive_subkey(key, stream_tag + ":send");
    recv_key_ = derive_subkey(key, stream_tag + ":recv");
  } else {
    send_key_ = derive_subkey(key, stream_tag + ":recv");
    recv_key_ = derive_subkey(key, stream_tag + ":send");
  }
}

Bytes StreamCipher::encrypt(std::span<const std::uint8_t> plaintext) {
  std::array<std::uint8_t, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES> nonce{};
  randombytes_buf(nonce.data(), nonce.size());

  Bytes out(nonce.size() + plaintext.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES);
  unsigned long long cipher_len = 0;
  if (crypto_aead_xchacha20poly1305_ietf_encrypt(out.data() + nonce.size(), &cipher_len, plaintext.data(), plaintext.size(),
                                                 nullptr, 0, nullptr, nonce.data(), send_key_.data()) != 0) {
    throw KikoError("encrypt failed");
  }
  std::copy(nonce.begin(), nonce.end(), out.begin());
  out.resize(nonce.size() + static_cast<std::size_t>(cipher_len));
  return out;
}

Bytes StreamCipher::decrypt(std::span<const std::uint8_t> ciphertext) {
  if (ciphertext.size() < crypto_aead_xchacha20poly1305_ietf_NPUBBYTES + crypto_aead_xchacha20poly1305_ietf_ABYTES) {
    throw KikoError("ciphertext too short");
  }
  auto nonce_size = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
  auto nonce = ciphertext.subspan(0, nonce_size);
  auto body = ciphertext.subspan(nonce_size);

  Bytes out(body.size());
  unsigned long long plain_len = 0;
  if (crypto_aead_xchacha20poly1305_ietf_decrypt(out.data(), &plain_len, nullptr, body.data(), body.size(), nullptr, 0,
                                                 nonce.data(), recv_key_.data()) != 0) {
    throw KikoError("decrypt authentication failed");
  }
  out.resize(static_cast<std::size_t>(plain_len));
  return out;
}

}  // namespace kiko
