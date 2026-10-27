#include "os_upload_policy.h"

#include "os_upload_text.h"

#include <algorithm>

namespace opensensor::upload {
namespace {

bool slug(std::string_view s, std::size_t max) {
  return !s.empty() && s.size() <= max && s.front() != '-' && s.back() != '-' &&
         std::ranges::all_of(s, [](char c) {
           return text::is_lower(c) || text::is_digit(c) || c == '-';
         });
}

bool prefix_ok(std::string_view p) {
  if (p.empty() || p.size() > 128 || p.back() != '/' || p.front() == '/' ||
      p.starts_with("dev/") || p.find("//") != std::string_view::npos ||
      p.find("..") != std::string_view::npos)
    return false;
  return std::ranges::all_of(p, [](char c) {
    return text::is_lower(c) || text::is_digit(c) || c == '-' || c == '_' ||
           c == '.' || c == '=' || c == '/';
  });
}

// Consumes `literal` from the front of `rest`.
bool take(std::string_view &rest, std::string_view literal) {
  if (!rest.starts_with(literal))
    return false;
  rest.remove_prefix(literal.size());
  return true;
}

// Consumes exactly `n` digits.
std::int64_t take_digits(std::string_view &rest, std::size_t n) {
  if (rest.size() < n)
    return -1;
  const std::int64_t v = text::parse_digits(rest.substr(0, n));
  rest.remove_prefix(n);
  return v;
}

// Consumes up to `stop`, requiring 1 to `max` bytes all matching `ok`.
template <typename Pred>
bool take_until(std::string_view &rest, char stop, std::size_t max, Pred ok) {
  const std::size_t at = rest.find(stop);
  if (at == 0 || at == std::string_view::npos || at > max)
    return false;
  if (!std::ranges::all_of(rest.substr(0, at), ok))
    return false;
  rest.remove_prefix(at + 1);
  return true;
}

} // namespace

bool safe_archive_name(std::string_view name) {
  if (name.size() < 9 || name.size() > kMaxArchiveName || name.front() == '/' ||
      !name.ends_with(".parquet"))
    return false;
  if (!std::ranges::all_of(name, [](char c) {
        return text::is_digit(c) || text::is_lower(c) || c == '-' || c == '.' ||
               c == '_' || c == '=' || c == '/';
      }))
    return false;
  return name.find("..") == std::string_view::npos &&
         name.find("//") == std::string_view::npos;
}

std::optional<FileTime> finalized_dated(std::string_view name) {
  if (!safe_archive_name(name))
    return std::nullopt;
  std::string_view rest = name;
  const auto id_char = [](char c) {
    return text::is_lower(c) || text::is_digit(c) || c == '-' || c == '_';
  };
  if (!take(rest, "thing=") || !take_until(rest, '/', 64, id_char))
    return std::nullopt;
  if (!take(rest, "year="))
    return std::nullopt;
  const std::int64_t year = take_digits(rest, 4);
  if (!take(rest, "/month="))
    return std::nullopt;
  const std::int64_t month = take_digits(rest, 2);
  if (!take(rest, "/day="))
    return std::nullopt;
  const std::int64_t day = take_digits(rest, 2);
  if (!take(rest, "/data_"))
    return std::nullopt;
  const std::int64_t hour = take_digits(rest, 2);
  const std::int64_t minute = take_digits(rest, 2);
  if (!take(rest, "_"))
    return std::nullopt;
  if (year < 2020 || year > 2099 || month < 1 || month > 12 || day < 1 ||
      day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59)
    return std::nullopt;
  if (!take_until(rest, '_', 32, text::is_lower_hex))
    return std::nullopt;
  // <first>-<last>-<attempt>.parquet, decimal counters.
  if (!take_until(rest, '-', 10, text::is_digit) ||
      !take_until(rest, '-', 10, text::is_digit) ||
      !take_until(rest, '.', 10, text::is_digit) || rest != "parquet")
    return std::nullopt;
  return FileTime{.year = static_cast<std::uint16_t>(year),
                  .month = static_cast<std::uint8_t>(month),
                  .day = static_cast<std::uint8_t>(day),
                  .hour = static_cast<std::uint8_t>(hour),
                  .minute = static_cast<std::uint8_t>(minute)};
}

std::expected<std::size_t, KeyError> object_key(const Destination &dest,
                                                std::string_view name,
                                                std::span<char> out) {
  if (!slug(dest.product, 64))
    return std::unexpected(KeyError::BadProduct);
  if (!slug(dest.board, 32))
    return std::unexpected(KeyError::BadBoard);
  if (dest.target == Target::Prod && !prefix_ok(dest.prod_prefix))
    return std::unexpected(KeyError::BadPrefix);
  if (dest.target == Target::Dev && !dest.prod_prefix.empty())
    return std::unexpected(KeyError::BadPrefix);
  if (!finalized_dated(name))
    return std::unexpected(KeyError::BadName);
  text::Writer w(out.size() > kMaxObjectKey ? out.first(kMaxObjectKey) : out);
  w.put(dest.product);
  w.put('/');
  if (dest.target == Target::Dev) {
    w.put(kDevPrefix);
    w.put(dest.board);
    w.put('/');
  } else {
    w.put(dest.prod_prefix);
  }
  w.put(name);
  if (!w.ok())
    return std::unexpected(KeyError::Overflow);
  if (!key_within_target(dest, w.view()))
    return std::unexpected(KeyError::OutsideTarget);
  return w.size();
}

bool key_within_target(const Destination &dest, std::string_view key) {
  if (key.empty() || key.size() > kMaxObjectKey ||
      key.find("..") != std::string_view::npos ||
      key.find("//") != std::string_view::npos)
    return false;
  std::string_view rest = key;
  if (!take(rest, dest.product) || !take(rest, "/"))
    return false;
  if (dest.target == Target::Dev)
    return take(rest, kDevPrefix) && take(rest, dest.board) &&
           take(rest, "/") && finalized_dated(rest).has_value();
  return !rest.starts_with("dev/") && take(rest, dest.prod_prefix) &&
         finalized_dated(rest).has_value();
}

void NextFile::offer(std::string_view candidate, std::uint64_t file_bytes) {
  const auto time = finalized_dated(candidate);
  if (!time || file_bytes == 0)
    return;
  const std::uint32_t order = time->order();
  if (found()) {
    if (order > order_)
      return;
    if (order == order_ && candidate >= name())
      return;
  }
  if (ledger_.contains(candidate))
    return;
  std::ranges::copy(candidate, name_.begin());
  size_ = candidate.size();
  bytes_ = file_bytes;
  order_ = order;
}

Gate decide(const PolicyState &state, const Inputs &in) {
  if (!in.enabled)
    return Gate::Disabled;
  if (state.stopped && state.stopped_key_generation == in.key_generation)
    return Gate::Stopped;
  if (!utc_trusted(in.clock))
    return Gate::ClockUntrusted;
  if (!in.network_up)
    return Gate::Offline;
  if (state.failures > 0 && in.mono_ms < state.retry_at_ms)
    return Gate::BackingOff;
  return Gate::Run;
}

std::uint32_t backoff_ms(std::uint32_t failures) {
  if (failures == 0)
    return 0;
  std::uint64_t delay = kBackoffFirstMs;
  for (std::uint32_t i = 1; i < failures && delay < kBackoffMaxMs; ++i)
    delay *= 2;
  return static_cast<std::uint32_t>(
      std::min<std::uint64_t>(delay, kBackoffMaxMs));
}

void apply(PolicyState &state, Outcome outcome, const Inputs &in) {
  if (state.stopped && state.stopped_key_generation != in.key_generation)
    state = PolicyState{};
  switch (outcome) {
  case Outcome::Uploaded:
  case Outcome::AlreadyUploaded:
  case Outcome::NothingPending:
    state.failures = 0;
    state.retry_at_ms = 0;
    return;
  case Outcome::Transient:
    if (state.failures < UINT32_MAX)
      ++state.failures;
    state.retry_at_ms = in.mono_ms + backoff_ms(state.failures);
    return;
  case Outcome::AuthRejected:
  case Outcome::RequestRejected:
  case Outcome::OutsideTarget:
    state.stopped = true;
    state.stopped_key_generation = in.key_generation;
    return;
  }
}

PutClass classify_put(int status, std::string_view code) {
  if (status >= 200 && status < 300)
    return PutClass::Created;
  if (status == 412)
    return PutClass::Exists;
  if ((status == 400 || status == 403) &&
      (code == "ExpiredToken" || code == "TokenRefreshRequired"))
    return PutClass::Expired;
  if (status == 401 || status == 403)
    return PutClass::Auth;
  if (status <= 0 || status == 408 || status == 409 || status == 429 ||
      (status >= 500 && status < 600))
    return PutClass::Transient;
  return PutClass::Rejected;
}

Outcome classify_sts_failure(int status) {
  if (status >= 400 && status < 500 && status != 408 && status != 429)
    return Outcome::AuthRejected;
  return Outcome::Transient;
}

} // namespace opensensor::upload
