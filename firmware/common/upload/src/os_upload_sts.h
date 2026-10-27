#pragma once

// STS AssumeRoleWithWebIdentity for the source.coop data proxy (#81). The
// board trades its long lived API key for one hour of S3 credentials. The
// key goes in the POST body, never in the URL, so it stays out of proxy and
// server access logs. The response parser is bounded, allocation free and
// rejects anything it does not fully understand.

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace opensensor::upload {

inline constexpr std::string_view kStsPath = "/.sts";
inline constexpr std::string_view kStsContentType =
    "application/x-www-form-urlencoded";
inline constexpr std::uint32_t kStsDurationSeconds = 3600;
inline constexpr std::size_t kMaxWebIdentityToken = 512;
inline constexpr std::size_t kStsBodyMax = 2048;

struct StsParams {
  std::string_view role_arn;
  std::string_view session_name;
  // The board API key. Printable ASCII with no spaces.
  std::string_view web_identity_token;
  std::uint32_t duration_s = kStsDurationSeconds;
};

enum class StsBuildError : std::uint8_t {
  BadRoleArn,
  BadSessionName,
  BadToken,
  BadDuration,
  Overflow,
};

// Writes the form encoded POST body and returns its length.
[[nodiscard]] std::expected<std::size_t, StsBuildError>
build_sts_body(const StsParams &params, std::span<char> out);

inline constexpr std::size_t kAccessKeyIdMax = 128;
inline constexpr std::size_t kSecretKeyMax = 128;
inline constexpr std::size_t kSessionTokenMax = 2048;

// Temporary credentials. Each field is NUL terminated inside its array.
struct Credentials {
  std::array<char, kAccessKeyIdMax + 1> access_key_id{};
  std::array<char, kSecretKeyMax + 1> secret_access_key{};
  std::array<char, kSessionTokenMax + 1> session_token{};
  std::int64_t expires_utc_s = 0;

  [[nodiscard]] bool present() const { return access_key_id[0] != '\0'; }
  [[nodiscard]] std::string_view key_id() const;
  [[nodiscard]] std::string_view secret() const;
  [[nodiscard]] std::string_view token() const;
  // Zeroes every field through a volatile store.
  void wipe();
};

enum class StsParseError : std::uint8_t {
  // The closing response element never arrived.
  Truncated,
  // Duplicate, nested or unbalanced elements, or bytes we do not accept.
  Malformed,
  MissingField,
  FieldTooLong,
  BadExpiration,
  // A well formed `<ErrorResponse>`.
  Rejected,
};

// Parses an AssumeRoleWithWebIdentityResponse. `out` is wiped on failure.
[[nodiscard]] std::expected<void, StsParseError>
parse_sts_response(std::string_view xml, Credentials &out);

// The first `<Code>` text in an error body, bounded to 64 bytes of
// [A-Za-z0-9.]. Empty when there is none.
[[nodiscard]] std::string_view error_code(std::string_view xml);

// ISO 8601 `YYYY-MM-DDTHH:MM:SS[.fff]Z` to Unix seconds, 2020 to 2099 only.
[[nodiscard]] std::expected<std::int64_t, StsParseError>
parse_iso8601(std::string_view text);

} // namespace opensensor::upload
