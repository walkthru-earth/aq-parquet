#include "os_upload_sts.h"

#include "os_upload_text.h"

#include <utc_clock.h>

#include <algorithm>

namespace opensensor::upload {
namespace {

constexpr std::size_t kMaxRoleArn = 256;
constexpr std::size_t kMaxResponse = 8192;
constexpr std::size_t kMaxErrorCode = 64;
constexpr std::string_view kResponseOpen = "<AssumeRoleWithWebIdentityResponse";
constexpr std::string_view kResponseClose =
    "</AssumeRoleWithWebIdentityResponse>";
constexpr std::string_view kErrorOpen = "<ErrorResponse";
constexpr std::string_view kErrorClose = "</ErrorResponse>";

bool session_char(char c) {
  return text::is_alnum(c) || c == '_' || c == '+' || c == '=' || c == ',' ||
         c == '.' || c == '@' || c == '-';
}

std::size_t count(std::string_view haystack, std::string_view needle) {
  std::size_t n = 0;
  for (std::size_t at = haystack.find(needle); at != std::string_view::npos;
       at = haystack.find(needle, at + needle.size()))
    ++n;
  return n;
}

// Form encodes straight into the body, the same rule as `encode_component`.
void put_param(text::Writer &w, std::string_view name, std::string_view value) {
  w.put(name);
  w.put('=');
  for (const char c : value) {
    if (text::unreserved(c)) {
      w.put(c);
      continue;
    }
    const auto byte = static_cast<std::uint8_t>(c);
    w.put('%');
    w.put(text::kUpperHex[byte >> 4]);
    w.put(text::kUpperHex[byte & 0x0f]);
  }
}

// The text of the single `<tag>` element inside `block`.
std::expected<std::string_view, StsParseError>
field(std::string_view block, std::string_view tag, std::size_t max) {
  std::array<char, 40> open{};
  std::array<char, 41> close{};
  text::Writer wo(open);
  wo.put('<');
  wo.put(tag);
  wo.put('>');
  text::Writer wc(close);
  wc.put("</");
  wc.put(tag);
  wc.put('>');
  if (!wo.ok() || !wc.ok())
    return std::unexpected(StsParseError::Malformed);
  const std::string_view o = wo.view();
  const std::string_view c = wc.view();
  const std::size_t opens = count(block, o);
  if (opens == 0)
    return std::unexpected(StsParseError::MissingField);
  if (opens != 1 || count(block, c) != 1)
    return std::unexpected(StsParseError::Malformed);
  const std::size_t begin = block.find(o) + o.size();
  const std::size_t end = block.find(c);
  if (end < begin)
    return std::unexpected(StsParseError::Malformed);
  const std::string_view value = block.substr(begin, end - begin);
  if (value.empty())
    return std::unexpected(StsParseError::MissingField);
  if (value.size() > max)
    return std::unexpected(StsParseError::FieldTooLong);
  if (!std::ranges::all_of(value, text::is_graph) ||
      value.find('<') != std::string_view::npos ||
      value.find('&') != std::string_view::npos)
    return std::unexpected(StsParseError::Malformed);
  return value;
}

template <std::size_t N>
void copy_field(std::array<char, N> &out, std::string_view value) {
  std::ranges::copy(value, out.begin());
  out[value.size()] = '\0';
}

template <std::size_t N>
std::string_view view_of(const std::array<char, N> &a) {
  const auto end = std::ranges::find(a, '\0');
  return {a.data(), static_cast<std::size_t>(end - a.begin())};
}

} // namespace

std::expected<std::size_t, StsBuildError>
build_sts_body(const StsParams &params, std::span<char> out) {
  const std::string_view arn = params.role_arn;
  if (arn.size() < 5 || arn.size() > kMaxRoleArn || !arn.starts_with("arn:") ||
      !std::ranges::all_of(arn, text::is_graph))
    return std::unexpected(StsBuildError::BadRoleArn);
  const std::string_view session = params.session_name;
  if (session.size() < 2 || session.size() > 64 ||
      !std::ranges::all_of(session, session_char))
    return std::unexpected(StsBuildError::BadSessionName);
  const std::string_view token = params.web_identity_token;
  if (token.empty() || token.size() > kMaxWebIdentityToken ||
      !std::ranges::all_of(token, text::is_graph))
    return std::unexpected(StsBuildError::BadToken);
  if (params.duration_s < 900 || params.duration_s > 43200)
    return std::unexpected(StsBuildError::BadDuration);

  // Parameters in byte order, the order AWS documents and signs them in.
  text::Writer w(out);
  w.put("Action=AssumeRoleWithWebIdentity&DurationSeconds=");
  w.put_uint(params.duration_s);
  w.put('&');
  put_param(w, "RoleArn", arn);
  w.put('&');
  put_param(w, "RoleSessionName", session);
  w.put("&Version=2011-06-15&");
  put_param(w, "WebIdentityToken", token);
  if (!w.ok())
    return std::unexpected(StsBuildError::Overflow);
  return w.size();
}

std::string_view Credentials::key_id() const { return view_of(access_key_id); }
std::string_view Credentials::secret() const {
  return view_of(secret_access_key);
}
std::string_view Credentials::token() const { return view_of(session_token); }

void Credentials::wipe() {
  text::secure_zero(access_key_id);
  text::secure_zero(secret_access_key);
  text::secure_zero(session_token);
  expires_utc_s = 0;
}

std::expected<std::int64_t, StsParseError> parse_iso8601(std::string_view t) {
  // 2026-10-27T09:00:00Z with an optional .fraction of 1 to 9 digits.
  if (t.size() < 20 || t.size() > 30 || t.back() != 'Z' || t[4] != '-' ||
      t[7] != '-' || t[10] != 'T' || t[13] != ':' || t[16] != ':')
    return std::unexpected(StsParseError::BadExpiration);
  if (t.size() > 20) {
    if (t[19] != '.' || t.size() == 21)
      return std::unexpected(StsParseError::BadExpiration);
    if (text::parse_digits(t.substr(20, t.size() - 21)) < 0)
      return std::unexpected(StsParseError::BadExpiration);
  }
  const std::int64_t year = text::parse_digits(t.substr(0, 4));
  const std::int64_t month = text::parse_digits(t.substr(5, 2));
  const std::int64_t day = text::parse_digits(t.substr(8, 2));
  const std::int64_t hour = text::parse_digits(t.substr(11, 2));
  const std::int64_t minute = text::parse_digits(t.substr(14, 2));
  const std::int64_t second = text::parse_digits(t.substr(17, 2));
  if (year < 2020 || year > 2099 || month < 1 || month > 12 || day < 1 ||
      day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
      second < 0 || second > 59)
    return std::unexpected(StsParseError::BadExpiration);
  const std::int64_t days =
      utc::days_from_civil(static_cast<int>(year), static_cast<unsigned>(month),
                           static_cast<unsigned>(day));
  const text::Civil back = text::civil_from_days(days);
  if (back.year != year || back.month != month || back.day != day)
    return std::unexpected(StsParseError::BadExpiration);
  return days * 86400 + hour * 3600 + minute * 60 + second;
}

std::expected<void, StsParseError> parse_sts_response(std::string_view xml,
                                                      Credentials &out) {
  out.wipe();
  const auto fail = [&out](StsParseError e) {
    out.wipe();
    return std::unexpected(e);
  };
  if (xml.size() > kMaxResponse)
    return fail(StsParseError::Malformed);
  if (std::ranges::any_of(xml, [](char c) {
        const auto b = static_cast<unsigned char>(c);
        return b == 0 || (b < 0x20 && b != '\t' && b != '\n' && b != '\r');
      }))
    return fail(StsParseError::Malformed);
  const bool error = xml.find(kErrorOpen) != std::string_view::npos;
  const bool closed = xml.find(kResponseClose) != std::string_view::npos;
  if (error && closed)
    return fail(StsParseError::Malformed);
  if (error)
    return fail(xml.find(kErrorClose) != std::string_view::npos
                    ? StsParseError::Rejected
                    : StsParseError::Truncated);
  if (!closed)
    return fail(StsParseError::Truncated);
  if (count(xml, kResponseOpen) != 1 || count(xml, kResponseClose) != 1 ||
      xml.find(kResponseOpen) > xml.find(kResponseClose))
    return fail(StsParseError::Malformed);

  constexpr std::string_view kOpen = "<Credentials>";
  constexpr std::string_view kClose = "</Credentials>";
  if (count(xml, kOpen) == 0)
    return fail(StsParseError::MissingField);
  if (count(xml, kOpen) != 1 || count(xml, kClose) != 1)
    return fail(StsParseError::Malformed);
  const std::size_t begin = xml.find(kOpen) + kOpen.size();
  const std::size_t end = xml.find(kClose);
  if (end < begin || end > xml.find(kResponseClose))
    return fail(StsParseError::Malformed);
  const std::string_view block = xml.substr(begin, end - begin);

  const auto key = field(block, "AccessKeyId", kAccessKeyIdMax);
  if (!key)
    return fail(key.error());
  if (!std::ranges::all_of(*key, text::is_alnum))
    return fail(StsParseError::Malformed);
  const auto secret = field(block, "SecretAccessKey", kSecretKeyMax);
  if (!secret)
    return fail(secret.error());
  const auto token = field(block, "SessionToken", kSessionTokenMax);
  if (!token)
    return fail(token.error());
  const auto expiration = field(block, "Expiration", 30);
  if (!expiration)
    return fail(expiration.error());
  const auto expires = parse_iso8601(*expiration);
  if (!expires)
    return fail(expires.error());

  copy_field(out.access_key_id, *key);
  copy_field(out.secret_access_key, *secret);
  copy_field(out.session_token, *token);
  out.expires_utc_s = *expires;
  return {};
}

std::string_view error_code(std::string_view xml) {
  constexpr std::string_view kOpen = "<Code>";
  constexpr std::string_view kClose = "</Code>";
  const std::size_t open = xml.find(kOpen);
  if (open == std::string_view::npos)
    return {};
  const std::size_t begin = open + kOpen.size();
  const std::size_t close = xml.find(kClose, begin);
  if (close == std::string_view::npos || close == begin ||
      close - begin > kMaxErrorCode)
    return {};
  const std::string_view code = xml.substr(begin, close - begin);
  if (!std::ranges::all_of(
          code, [](char c) { return text::is_alnum(c) || c == '.'; }))
    return {};
  return code;
}

} // namespace opensensor::upload
