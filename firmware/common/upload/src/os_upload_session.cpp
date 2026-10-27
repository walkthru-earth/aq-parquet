#include "os_upload_session.h"

#include "os_upload_text.h"

#include <sync_codec.h>

#include <algorithm>

namespace opensensor::upload {
namespace {

constexpr std::size_t kPathMax = 3 * (64 + 1 + kMaxObjectKey) + 2;
constexpr std::size_t kAuthorizationMax = 640;

// Streams one archive file and keeps a running crc32 of what was sent.
class ArchiveBody final : public BodySource {
public:
  ArchiveBody(Archive &archive, std::string_view name)
      : archive_(archive), name_(name) {}

  std::expected<std::size_t, IoError>
  read(std::uint64_t offset, std::span<std::uint8_t> out) override {
    auto got = archive_.read(name_, offset, out);
    if (got && offset == sent_) {
      crc_ = sync::crc32_update(crc_, out.data(), *got);
      sent_ += *got;
    }
    return got;
  }

  [[nodiscard]] std::uint32_t crc() const { return crc_ ^ 0xffffffffU; }
  [[nodiscard]] std::uint64_t sent() const { return sent_; }

private:
  Archive &archive_;
  std::string_view name_;
  std::uint32_t crc_ = 0xffffffffU;
  std::uint64_t sent_ = 0;
};

// Fixed size form bodies, the STS request.
class SpanBody final : public BodySource {
public:
  explicit SpanBody(std::span<const char> data) : data_(data) {}

  std::expected<std::size_t, IoError>
  read(std::uint64_t offset, std::span<std::uint8_t> out) override {
    if (offset > data_.size())
      return std::unexpected(IoError::Failed);
    const std::size_t n =
        std::min<std::size_t>(out.size(), data_.size() - offset);
    for (std::size_t i = 0; i < n; ++i)
      out[i] = static_cast<std::uint8_t>(data_[offset + i]);
    return n;
  }

private:
  std::span<const char> data_;
};

} // namespace

StepResult Session::step(const Inputs &in, std::int64_t utc_s) {
  const Gate gate = decide(state_, in);
  if (gate != Gate::Run)
    return {.gate = gate, .outcome = Outcome::NothingPending};
  if (state_.stopped)
    // The key changed since the stop, so the old credentials go too.
    creds_.wipe();
  const Outcome outcome = run(utc_s);
  apply(state_, outcome, in);
  return {.gate = gate, .outcome = outcome};
}

Outcome Session::run(std::int64_t utc_s) {
  NextFile next(ledger_);
  if (!archive_.list(next))
    return Outcome::Transient;
  if (!next.found())
    return Outcome::NothingPending;
  // NextFile owns the name buffer, keep it alive for the whole upload.
  return put(next.name(), next.bytes(), utc_s);
}

std::optional<Outcome> Session::refresh(std::int64_t utc_s) {
  creds_.wipe();
  std::array<char, kStsBodyMax> body{};
  const auto size = build_sts_body({.role_arn = config_.role_arn,
                                    .session_name = config_.session_name,
                                    .web_identity_token = config_.api_key,
                                    .duration_s = kStsDurationSeconds},
                                   body);
  if (!size) {
    text::secure_zero(body);
    return Outcome::RequestRejected;
  }
  const std::array headers = {
      Header{.name = "content-type", .value = kStsContentType},
  };
  SpanBody source(std::span<const char>(body.data(), *size));
  auto response = transport_.send({.method = "POST",
                                   .host = config_.host,
                                   .path = kStsPath,
                                   .headers = headers,
                                   .body_bytes = *size,
                                   .body = &source},
                                  response_);
  text::secure_zero(body);
  if (!response)
    return Outcome::Transient;
  const std::string_view xml(response_.data(), response->body_bytes);
  if (response->status < 200 || response->status >= 300 ||
      response->truncated) {
    const Outcome outcome = classify_sts_failure(response->status);
    text::secure_zero(response_);
    return outcome;
  }
  const auto parsed = parse_sts_response(xml, creds_);
  text::secure_zero(response_);
  if (!parsed)
    return Outcome::Transient;
  if (creds_.expires_utc_s <= utc_s + kRefreshMarginSeconds) {
    creds_.wipe();
    return Outcome::Transient;
  }
  return std::nullopt;
}

Outcome Session::put(std::string_view name, std::uint64_t bytes,
                     std::int64_t utc_s) {
  std::array<char, kMaxObjectKey> key{};
  const auto key_size = object_key(config_.destination, name, key);
  if (!key_size)
    return key_size.error() == KeyError::OutsideTarget
               ? Outcome::OutsideTarget
               : Outcome::RequestRejected;
  const std::string_view object(key.data(), *key_size);
  if (!key_within_target(config_.destination, object))
    return Outcome::OutsideTarget;

  if (!creds_.present() ||
      creds_.expires_utc_s <= utc_s + kRefreshMarginSeconds) {
    if (const auto failed = refresh(utc_s))
      return *failed;
  }

  // First pass, the payload hash for the signature and the crc for the
  // ledger. The size must match the listing.
  hash_.begin();
  std::uint32_t crc = 0xffffffffU;
  std::uint64_t offset = 0;
  while (offset < bytes) {
    const auto got = archive_.read(name, offset, chunk_);
    if (!got || *got == 0)
      return Outcome::Transient;
    hash_.update(std::span<const std::uint8_t>(chunk_.data(), *got));
    crc = sync::crc32_update(crc, chunk_.data(), *got);
    offset += *got;
  }
  crc ^= 0xffffffffU;
  Digest payload{};
  if (offset != bytes || !hash_.finish(payload))
    return Outcome::Transient;
  HexDigest payload_hex{};
  to_hex(payload, payload_hex);

  const auto date = format_amz_date(utc_s);
  if (!date)
    return Outcome::Transient;
  const std::string_view amz_date(date->data(), date->size());

  // "/<bucket>/<key>", every segment encoded once.
  std::array<char, 1 + 64 + 1 + kMaxObjectKey> raw_path{};
  text::Writer raw(raw_path);
  raw.put('/');
  raw.put(config_.bucket);
  raw.put('/');
  raw.put(object);
  if (!raw.ok() || config_.bucket.empty() || config_.bucket.size() > 64 ||
      config_.bucket.find('/') != std::string_view::npos)
    return Outcome::RequestRejected;
  std::array<char, kPathMax> path{};
  const auto path_size = encode_path(raw.view(), path);
  if (!path_size)
    return Outcome::RequestRejected;
  const std::string_view encoded(path.data(), *path_size);

  const std::string_view hex(payload_hex.data(), payload_hex.size());
  // Named, so the signed list and the sent list cannot drift by index.
  const Header content_type{.name = "content-type",
                            .value = kParquetContentType};
  const Header host{.name = "host", .value = config_.host};
  const Header if_none_match{.name = "if-none-match", .value = "*"};
  const Header content_sha{.name = "x-amz-content-sha256", .value = hex};
  const Header date_header{.name = "x-amz-date", .value = amz_date};
  const Header token{.name = "x-amz-security-token", .value = creds_.token()};
  const std::array headers = {content_type, host,        if_none_match,
                              content_sha,  date_header, token};
  std::array<char, kAuthorizationMax> auth_header{};
  const auto auth_size = authorization(hash_,
                                       {.method = "PUT",
                                        .canonical_uri = encoded,
                                        .canonical_query = "",
                                        .headers = headers,
                                        .payload_sha256_hex = hex,
                                        .amz_date = amz_date,
                                        .region = config_.region,
                                        .service = "s3",
                                        .access_key_id = creds_.key_id(),
                                        .secret_access_key = creds_.secret()},
                                       auth_header);
  if (!auth_size) {
    text::secure_zero(auth_header);
    return Outcome::RequestRejected;
  }

  // Everything signed except Host, which the transport writes, plus the
  // Authorization header itself.
  const std::array sent = {
      content_type,
      if_none_match,
      content_sha,
      date_header,
      token,
      Header{.name = "authorization",
             .value = std::string_view(auth_header.data(), *auth_size)},
  };
  ArchiveBody body(archive_, name);
  auto response = transport_.send({.method = "PUT",
                                   .host = config_.host,
                                   .path = encoded,
                                   .headers = sent,
                                   .body_bytes = bytes,
                                   .body = &body},
                                  response_);
  text::secure_zero(auth_header);
  if (!response)
    return Outcome::Transient;
  const std::string_view reply(response_.data(), response->body_bytes);
  const PutClass put_class = classify_put(response->status, error_code(reply));
  switch (put_class) {
  case PutClass::Created:
  case PutClass::Exists: {
    // S3 checked the signed body hash, so a 2xx means it holds the same
    // bytes the first pass hashed. A stream that did not match is retried.
    if (put_class == PutClass::Created &&
        (body.sent() != bytes || body.crc() != crc))
      return Outcome::Transient;
    const LedgerEntry entry{
        .name = name,
        .bytes = bytes,
        .crc32 = crc,
        .etag = put_class == PutClass::Created
                    ? sanitize_etag(response->etag_view())
                    : std::string_view("-"),
    };
    if (!ledger_.append(entry))
      // The object is up. The next run gets a 412 and records it then.
      return Outcome::Transient;
    return put_class == PutClass::Created ? Outcome::Uploaded
                                          : Outcome::AlreadyUploaded;
  }
  case PutClass::Expired:
    creds_.wipe();
    return Outcome::Transient;
  case PutClass::Auth:
    creds_.wipe();
    return Outcome::AuthRejected;
  case PutClass::Transient:
    return Outcome::Transient;
  case PutClass::Rejected:
    return Outcome::RequestRejected;
  }
  return Outcome::Transient;
}

} // namespace opensensor::upload
