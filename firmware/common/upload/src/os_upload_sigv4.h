#pragma once

// AWS Signature Version 4 for S3 PutObject and other header signed requests
// (#81). Pure C++ with no heap. SHA-256 comes in through `Sha256`, so the
// device plugs in PSA crypto and the host tests plug in a reference hash.
// HMAC-SHA256 is built here on top of it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace opensensor::upload {

inline constexpr std::size_t kSha256Bytes = 32;
inline constexpr std::size_t kSha256BlockBytes = 64;
using Digest = std::array<std::uint8_t, kSha256Bytes>;
// 64 lowercase hex characters, not NUL terminated.
using HexDigest = std::array<char, 2 * kSha256Bytes>;

// Streaming SHA-256. `update` latches any backend failure and `finish`
// reports it, so callers check one result per digest.
class Sha256 {
public:
  virtual void begin() = 0;
  virtual void update(std::span<const std::uint8_t> data) = 0;
  [[nodiscard]] virtual bool finish(Digest &out) = 0;

  void update(std::string_view text) {
    update(std::span(reinterpret_cast<const std::uint8_t *>(text.data()),
                     text.size()));
  }

protected:
  ~Sha256() = default;
};

[[nodiscard]] bool sha256(Sha256 &hash, std::span<const std::uint8_t> data,
                          Digest &out);
// RFC 2104 HMAC over `hash`. Keys longer than one block are hashed first.
[[nodiscard]] bool hmac_sha256(Sha256 &hash, std::span<const std::uint8_t> key,
                               std::span<const std::uint8_t> message,
                               Digest &out);
void to_hex(std::span<const std::uint8_t, kSha256Bytes> digest, HexDigest &out);

// One request header as it is signed. The name is lowercase [a-z0-9-], the
// value has no leading, trailing or doubled spaces and no control bytes.
struct Header {
  std::string_view name;
  std::string_view value;
};

struct SignRequest {
  std::string_view method;
  // Already URI encoded, see `encode_path`. S3 does not encode it twice.
  std::string_view canonical_uri;
  // Already canonical (sorted and encoded), usually empty.
  std::string_view canonical_query;
  // Strictly sorted by name. Must hold `host` and `x-amz-date`.
  std::span<const Header> headers;
  // 64 lowercase hex characters of the body hash.
  std::string_view payload_sha256_hex;
  // YYYYMMDDTHHMMSSZ, equal to the `x-amz-date` header.
  std::string_view amz_date;
  std::string_view region;
  std::string_view service;
  std::string_view access_key_id;
  std::string_view secret_access_key;
};

enum class SignError : std::uint8_t {
  BadHeaders,
  BadDate,
  BadScope,
  BadPayloadHash,
  BadCredentials,
  Hash,
  Overflow,
};

// The 64 hex signature of `request`.
[[nodiscard]] std::expected<HexDigest, SignError>
signature(Sha256 &hash, const SignRequest &request);

// Writes the full `Authorization` header value into `out` and returns its
// length. `out` gets a final NUL when it has room for one.
[[nodiscard]] std::expected<std::size_t, SignError>
authorization(Sha256 &hash, const SignRequest &request, std::span<char> out);

// Seconds since the Unix epoch to the 16 character YYYYMMDDTHHMMSSZ form.
// Only years 2020 to 2099 are accepted, the range utc_clock.h supports.
[[nodiscard]] std::expected<std::array<char, 16>, SignError>
format_amz_date(std::int64_t utc_seconds);

// S3 path encoding. Keeps A-Z a-z 0-9 - . _ ~ and '/', encodes every other
// byte as %XX with uppercase hex.
[[nodiscard]] std::expected<std::size_t, SignError>
encode_path(std::string_view in, std::span<char> out);
// Form and query encoding, the same as `encode_path` but '/' is encoded too.
[[nodiscard]] std::expected<std::size_t, SignError>
encode_component(std::string_view in, std::span<char> out);

} // namespace opensensor::upload
