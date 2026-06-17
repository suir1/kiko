#include "pake.hpp"

#include "protocol.hpp"

#include <sodium.h>

#include <array>
#include <cstring>
#include <string>

namespace kiko {
namespace {

using Point = std::array<std::uint8_t, crypto_core_ristretto255_BYTES>;
using Scalar = std::array<std::uint8_t, crypto_core_ristretto255_SCALARBYTES>;

struct SodiumInit {
  SodiumInit() {
    if (sodium_init() < 0) throw KikoError("libsodium init failed");
  }
};

const SodiumInit& sodium_ready() {
  static const SodiumInit init;
  return init;
}

Point hash_to_point(const char* label) {
  std::array<std::uint8_t, 64> hash{};
  crypto_generichash(hash.data(), hash.size(), reinterpret_cast<const unsigned char*>(label), std::strlen(label), nullptr, 0);
  Point point{};
  crypto_core_ristretto255_from_hash(point.data(), hash.data());
  return point;
}

Scalar hash_to_scalar(const std::string& material) {
  std::array<std::uint8_t, 64> hash{};
  crypto_generichash(hash.data(), hash.size(), reinterpret_cast<const unsigned char*>(material.data()), material.size(), nullptr,
                     0);
  Scalar scalar{};
  crypto_core_ristretto255_scalar_reduce(scalar.data(), hash.data());
  return scalar;
}

Point scalar_base(const Scalar& scalar) {
  Point point{};
  if (crypto_scalarmult_ristretto255_base(point.data(), scalar.data()) != 0) throw KikoError("scalar base failed");
  return point;
}

Point scalar_mul(const Scalar& scalar, const Point& point) {
  Point out{};
  if (crypto_scalarmult_ristretto255(out.data(), scalar.data(), point.data()) != 0) throw KikoError("scalar mul failed");
  return out;
}

Point point_add(const Point& a, const Point& b) {
  Point out{};
  crypto_core_ristretto255_add(out.data(), a.data(), b.data());
  return out;
}

Point point_sub(const Point& a, const Point& b) {
  Point out{};
  crypto_core_ristretto255_sub(out.data(), a.data(), b.data());
  return out;
}

void require_valid_point(const Point& point) {
  if (crypto_core_ristretto255_is_valid_point(point.data()) != 1) throw KikoError("invalid ristretto255 point");
}

Bytes span_of(const std::string& value) { return Bytes(value.begin(), value.end()); }

}  // namespace

CodeParts split_code(const std::string& code) {
  auto pos = code.find('-');
  if (pos == std::string::npos) return CodeParts{code, code};
  return CodeParts{code.substr(0, pos), code.substr(pos + 1)};
}

std::string room_token(const std::string& code) {
  auto material = "kiko-room:" + split_code(code).room_label;
  auto digest = sha256(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(material.data()), material.size()));
  return hex_encode(digest);
}

SessionKey perform_handshake(TcpSocket& channel, Role role, const std::string& code) {
  sodium_ready();

  // CPace-style compact PAKE over Ristretto255 (HashToElement + two-sided SPAKE2 masks).
  const auto point_m = hash_to_point("kiko-cpace-M");
  const auto point_n = hash_to_point("kiko-cpace-N");
  const auto w = hash_to_scalar("kiko-pake:" + split_code(code).secret);

  const Point& self_mask = role == Role::Sender ? point_m : point_n;
  const Point& peer_mask = role == Role::Sender ? point_n : point_m;

  Scalar x{};
  randombytes_buf(x.data(), x.size());
  crypto_core_ristretto255_scalar_reduce(x.data(), x.data());
  if (sodium_is_zero(x.data(), x.size()) == 1) x[0] = 1;

  auto self_point = point_add(scalar_base(x), scalar_mul(w, self_mask));
  send_frame(channel, Bytes(self_point.begin(), self_point.end()));

  auto peer_frame = recv_frame(channel);
  if (!peer_frame || peer_frame->size() != point_m.size()) throw KikoError("peer closed during handshake");
  Point peer_point{};
  std::copy(peer_frame->begin(), peer_frame->end(), peer_point.begin());
  require_valid_point(peer_point);

  auto shared = scalar_mul(x, point_sub(peer_point, scalar_mul(w, peer_mask)));

  const Point& sender_elem = role == Role::Sender ? self_point : peer_point;
  const Point& receiver_elem = role == Role::Sender ? peer_point : self_point;

  Bytes transcript;
  auto append = [&transcript](const auto& part) { transcript.insert(transcript.end(), part.begin(), part.end()); };
  append(point_m);
  append(point_n);
  append(sender_elem);
  append(receiver_elem);
  append(shared);
  append(w);

  auto master = sha256(transcript);
  auto salt = span_of("kiko-cpace-v1");
  auto session_okm = hkdf_sha256(master, salt, span_of("session"), 32);
  auto kc_sender = hkdf_sha256(master, salt, span_of("confirm-sender"), 32);
  auto kc_receiver = hkdf_sha256(master, salt, span_of("confirm-receiver"), 32);

  auto confirm_label = span_of("kiko-confirm");
  auto mac_sender = hmac_sha256(kc_sender, confirm_label);
  auto mac_receiver = hmac_sha256(kc_receiver, confirm_label);

  const Bytes& my_mac = role == Role::Sender ? mac_sender : mac_receiver;
  const Bytes& expected_mac = role == Role::Sender ? mac_receiver : mac_sender;

  send_frame(channel, my_mac);
  auto peer_mac = recv_frame(channel);
  if (!peer_mac) throw KikoError("peer closed during key confirmation");
  if (!constant_time_equal(*peer_mac, expected_mac)) {
    throw KikoError("key confirmation failed: wrong code or tampering detected");
  }

  SessionKey key{};
  std::copy(session_okm.begin(), session_okm.end(), key.begin());
  return key;
}

}  // namespace kiko
