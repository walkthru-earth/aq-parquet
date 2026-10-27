#include "os_upload_sigv4.h"

#include "os_upload_text.h"

#include <utc_clock.h>

#include <algorithm>
#include <cstring>

namespace opensensor::upload {
namespace {

constexpr std::string_view kAlgorithm = "AWS4-HMAC-SHA256";
constexpr std::string_view kTerminator = "aws4_request";
constexpr std::size_t kMaxScopePart = 32;
constexpr std::size_t kMaxKeyId = 128;
constexpr std::size_t kMaxSecret = 128;
constexpr std::size_t kMaxHeaderName = 64;
constexpr std::size_t kMaxHeaderValue = 4096;
constexpr std::size_t kScopeMax = 8 + 3 * (kMaxScopePart + 1) + 16;
constexpr std::size_t kStringToSignMax = 17 + 17 + kScopeMax + 1 + 64;

std::span<const std::uint8_t> bytes(std::string_view text) {
  return {reinterpret_cast<const std::uint8_t *>(text.data()), text.size()};
}

bool header_name_ok(std::string_view name) {
  if (name.empty() || name.size() > kMaxHeaderName)
    return false;
  return std::ranges::all_of(name, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
  });
}

bool header_value_ok(std::string_view value) {
  if (value.size() > kMaxHeaderValue)
    return false;
  if (!value.empty() && (value.front() == ' ' || value.back() == ' '))
    return false;
  for (std::size_t i = 0; i < value.size(); ++i) {
    const char c = value[i];
    if (c < 0x20 || c > 0x7e)
      return false;
    if (c == ' ' && i > 0 && value[i - 1] == ' ')
      return false;
  }
  return true;
}

bool scope_part_ok(std::string_view part) {
  if (part.empty() || part.size() > kMaxScopePart)
    return false;
  return std::ranges::all_of(part, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
  });
}

bool amz_date_ok(std::string_view date) {
  if (date.size() != 16 || date[8] != 'T' || date[15] != 'Z')
    return false;
  for (std::size_t i = 0; i < 15; ++i)
    if (i != 8 && !text::is_digit(date[i]))
      return false;
  return true;
}

bool encoded_ok(std::string_view text, bool query) {
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (text::unreserved(c) || c == '/' || (query && (c == '=' || c == '&')))
      continue;
    if (c == '%' && i + 2 < text.size() && text::is_upper_hex(text[i + 1]) &&
        text::is_upper_hex(text[i + 2])) {
      i += 2;
      continue;
    }
    return false;
  }
  return true;
}

std::expected<void, SignError> validate(const SignRequest &r) {
  if (r.method.empty() || r.method.size() > 10 ||
      !std::ranges::all_of(r.method,
                           [](char c) { return c >= 'A' && c <= 'Z'; }))
    return std::unexpected(SignError::BadHeaders);
  if (r.canonical_uri.empty() || r.canonical_uri.front() != '/' ||
      !encoded_ok(r.canonical_uri, false) ||
      !encoded_ok(r.canonical_query, true))
    return std::unexpected(SignError::BadHeaders);
  if (!amz_date_ok(r.amz_date))
    return std::unexpected(SignError::BadDate);
  if (!scope_part_ok(r.region) || !scope_part_ok(r.service))
    return std::unexpected(SignError::BadScope);
  if (r.payload_sha256_hex.size() != 64 ||
      !std::ranges::all_of(r.payload_sha256_hex, text::is_lower_hex))
    return std::unexpected(SignError::BadPayloadHash);
  if (r.access_key_id.empty() || r.access_key_id.size() > kMaxKeyId ||
      !std::ranges::all_of(r.access_key_id, text::is_alnum) ||
      r.secret_access_key.empty() || r.secret_access_key.size() > kMaxSecret ||
      !std::ranges::all_of(r.secret_access_key, text::is_graph))
    return std::unexpected(SignError::BadCredentials);
  bool host = false;
  bool date = false;
  for (std::size_t i = 0; i < r.headers.size(); ++i) {
    const Header &h = r.headers[i];
    if (!header_name_ok(h.name) || !header_value_ok(h.value))
      return std::unexpected(SignError::BadHeaders);
    if (i > 0 && !(r.headers[i - 1].name < h.name))
      return std::unexpected(SignError::BadHeaders);
    if (h.name == "host")
      host = !h.value.empty();
    if (h.name == "x-amz-date") {
      if (h.value != r.amz_date)
        return std::unexpected(SignError::BadDate);
      date = true;
    }
  }
  if (!host || !date)
    return std::unexpected(SignError::BadHeaders);
  return {};
}

// Credential scope, YYYYMMDD/region/service/aws4_request.
std::size_t write_scope(const SignRequest &r, std::span<char> out) {
  text::Writer w(out);
  w.put(r.amz_date.substr(0, 8));
  w.put('/');
  w.put(r.region);
  w.put('/');
  w.put(r.service);
  w.put('/');
  w.put(kTerminator);
  return w.ok() ? w.size() : 0;
}

void put_signed_headers(Sha256 &hash, std::span<const Header> headers) {
  for (std::size_t i = 0; i < headers.size(); ++i) {
    if (i > 0)
      hash.update(std::string_view(";"));
    hash.update(headers[i].name);
  }
}

} // namespace

bool sha256(Sha256 &hash, std::span<const std::uint8_t> data, Digest &out) {
  hash.begin();
  hash.update(data);
  return hash.finish(out);
}

bool hmac_sha256(Sha256 &hash, std::span<const std::uint8_t> key,
                 std::span<const std::uint8_t> message, Digest &out) {
  std::array<std::uint8_t, kSha256BlockBytes> block{};
  if (key.size() > block.size()) {
    Digest hashed{};
    if (!sha256(hash, key, hashed))
      return false;
    std::ranges::copy(hashed, block.begin());
    text::secure_zero(hashed);
  } else {
    std::ranges::copy(key, block.begin());
  }
  std::array<std::uint8_t, kSha256BlockBytes> pad{};
  for (std::size_t i = 0; i < block.size(); ++i)
    pad[i] = block[i] ^ 0x36;
  Digest inner{};
  hash.begin();
  hash.update(pad);
  hash.update(message);
  bool ok = hash.finish(inner);
  for (std::size_t i = 0; i < block.size(); ++i)
    pad[i] = block[i] ^ 0x5c;
  hash.begin();
  hash.update(pad);
  hash.update(inner);
  ok = hash.finish(out) && ok;
  text::secure_zero(block);
  text::secure_zero(pad);
  text::secure_zero(inner);
  return ok;
}

void to_hex(std::span<const std::uint8_t, kSha256Bytes> digest,
            HexDigest &out) {
  for (std::size_t i = 0; i < digest.size(); ++i) {
    out[2 * i] = text::kLowerHex[digest[i] >> 4];
    out[2 * i + 1] = text::kLowerHex[digest[i] & 0x0f];
  }
}

std::expected<HexDigest, SignError> signature(Sha256 &hash,
                                              const SignRequest &r) {
  if (auto valid = validate(r); !valid)
    return std::unexpected(valid.error());

  // Canonical request, hashed as it is produced so it needs no buffer.
  hash.begin();
  hash.update(r.method);
  hash.update(std::string_view("\n"));
  hash.update(r.canonical_uri);
  hash.update(std::string_view("\n"));
  hash.update(r.canonical_query);
  hash.update(std::string_view("\n"));
  for (const Header &h : r.headers) {
    hash.update(h.name);
    hash.update(std::string_view(":"));
    hash.update(h.value);
    hash.update(std::string_view("\n"));
  }
  hash.update(std::string_view("\n"));
  put_signed_headers(hash, r.headers);
  hash.update(std::string_view("\n"));
  hash.update(r.payload_sha256_hex);
  Digest canonical{};
  if (!hash.finish(canonical))
    return std::unexpected(SignError::Hash);
  HexDigest canonical_hex{};
  to_hex(canonical, canonical_hex);

  std::array<char, kScopeMax> scope{};
  const std::size_t scope_size = write_scope(r, scope);
  if (scope_size == 0)
    return std::unexpected(SignError::Overflow);

  std::array<char, kStringToSignMax> to_sign{};
  text::Writer w(to_sign);
  w.put(kAlgorithm);
  w.put('\n');
  w.put(r.amz_date);
  w.put('\n');
  w.put(std::string_view(scope.data(), scope_size));
  w.put('\n');
  w.put(std::string_view(canonical_hex.data(), canonical_hex.size()));
  if (!w.ok())
    return std::unexpected(SignError::Overflow);

  // The signing key chain, every intermediate key wiped before return.
  std::array<std::uint8_t, 4 + kMaxSecret> secret{};
  std::memcpy(secret.data(), "AWS4", 4);
  std::memcpy(secret.data() + 4, r.secret_access_key.data(),
              r.secret_access_key.size());
  const std::span<const std::uint8_t> first(secret.data(),
                                            4 + r.secret_access_key.size());
  Digest key{};
  Digest next{};
  bool ok = hmac_sha256(hash, first, bytes(r.amz_date.substr(0, 8)), key);
  ok = ok && hmac_sha256(hash, key, bytes(r.region), next);
  ok = ok && hmac_sha256(hash, next, bytes(r.service), key);
  ok = ok && hmac_sha256(hash, key, bytes(kTerminator), next);
  Digest mac{};
  ok =
      ok && hmac_sha256(hash, next,
                        bytes(std::string_view(to_sign.data(), w.size())), mac);
  text::secure_zero(secret);
  text::secure_zero(key);
  text::secure_zero(next);
  if (!ok)
    return std::unexpected(SignError::Hash);
  HexDigest out{};
  to_hex(mac, out);
  return out;
}

std::expected<std::size_t, SignError>
authorization(Sha256 &hash, const SignRequest &r, std::span<char> out) {
  auto sig = signature(hash, r);
  if (!sig)
    return std::unexpected(sig.error());
  std::array<char, kScopeMax> scope{};
  const std::size_t scope_size = write_scope(r, scope);
  text::Writer w(out);
  w.put(kAlgorithm);
  w.put(" Credential=");
  w.put(r.access_key_id);
  w.put('/');
  w.put(std::string_view(scope.data(), scope_size));
  w.put(", SignedHeaders=");
  for (std::size_t i = 0; i < r.headers.size(); ++i) {
    if (i > 0)
      w.put(';');
    w.put(r.headers[i].name);
  }
  w.put(", Signature=");
  w.put(std::string_view(sig->data(), sig->size()));
  if (!w.terminate())
    return std::unexpected(SignError::Overflow);
  return w.size();
}

std::expected<std::array<char, 16>, SignError>
format_amz_date(std::int64_t utc_seconds) {
  // 2020-01-01 to 2099-12-31, inside the range utc_clock.h supports.
  if (utc_seconds < utc::kMinEpochSeconds ||
      utc_seconds >= utc::kMaxEpochSeconds)
    return std::unexpected(SignError::BadDate);
  const std::int64_t days = utc_seconds / 86400;
  const std::int64_t rest = utc_seconds % 86400;
  const text::Civil civil = text::civil_from_days(days);
  std::array<char, 16> out{};
  text::put_digits(std::span(out).subspan(0, 4), civil.year);
  text::put_digits(std::span(out).subspan(4, 2), civil.month);
  text::put_digits(std::span(out).subspan(6, 2), civil.day);
  out[8] = 'T';
  text::put_digits(std::span(out).subspan(9, 2), static_cast<int>(rest / 3600));
  text::put_digits(std::span(out).subspan(11, 2),
                   static_cast<int>(rest / 60 % 60));
  text::put_digits(std::span(out).subspan(13, 2), static_cast<int>(rest % 60));
  out[15] = 'Z';
  return out;
}

namespace {
std::expected<std::size_t, SignError>
encode(std::string_view in, std::span<char> out, bool keep_slash) {
  text::Writer w(out);
  for (const char c : in) {
    if (text::unreserved(c) || (keep_slash && c == '/')) {
      w.put(c);
      continue;
    }
    const auto byte = static_cast<std::uint8_t>(c);
    w.put('%');
    w.put(text::kUpperHex[byte >> 4]);
    w.put(text::kUpperHex[byte & 0x0f]);
  }
  if (!w.terminate())
    return std::unexpected(SignError::Overflow);
  return w.size();
}
} // namespace

std::expected<std::size_t, SignError> encode_path(std::string_view in,
                                                  std::span<char> out) {
  return encode(in, out, true);
}

std::expected<std::size_t, SignError> encode_component(std::string_view in,
                                                       std::span<char> out) {
  return encode(in, out, false);
}

} // namespace opensensor::upload
