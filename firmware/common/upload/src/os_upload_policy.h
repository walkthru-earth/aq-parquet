#pragma once

// Upload policy for the device uploader (#81). Pure functions with no clock,
// file or network access, so every rule is a host test.
//
// Rules
//   1. Only finalized dated archive files, oldest first, one at a time.
//   2. Nothing runs until UTC is trusted (host or network time). Wi-Fi
//      association alone never counts as a clock.
//   3. Errors back off exponentially.
//   4. A 401 or 403 stops the uploader until the key changes, so a revoked
//      key never loops.
//   5. The `Dev` target can never write outside the dev prefix.

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace opensensor::upload {

// The one place the dev prefix lives. Needs owner confirmation (#81).
inline constexpr std::string_view kDevPrefix = "dev/opensensor-space-firmware/";
inline constexpr std::size_t kMaxArchiveName = 384;
inline constexpr std::size_t kMaxObjectKey = 512;

enum class Target : std::uint8_t { Dev, Prod };

struct Destination {
  Target target = Target::Dev;
  // The source.coop product, the first key segment, like "opensensor-space".
  std::string_view product;
  // Lowercase [a-z0-9-], like "cores3" or "waveshare-sim7670g".
  std::string_view board;
  // Prod only. Ends in '/', never starts with "dev/". Waits on the owner.
  std::string_view prod_prefix;
};

enum class KeyError : std::uint8_t {
  BadProduct,
  BadBoard,
  BadPrefix,
  BadName,
  Overflow,
  OutsideTarget,
};

// "<product>/dev/opensensor-space-firmware/<board>/<name>" for Dev and
// "<product>/<prod_prefix><name>" for Prod. `name` is the SD relative
// archive name, used verbatim (decision 16). Not NUL terminated.
[[nodiscard]] std::expected<std::size_t, KeyError>
object_key(const Destination &dest, std::string_view name, std::span<char> out);

// Second guard, run on the final key right before signing.
[[nodiscard]] bool key_within_target(const Destination &dest,
                                     std::string_view key);

// The same character rules as sync::safe_parquet_name, for a string_view.
[[nodiscard]] bool safe_archive_name(std::string_view name);

// Date and minute of a finalized dated archive file.
struct FileTime {
  std::uint16_t year;
  std::uint8_t month;
  std::uint8_t day;
  std::uint8_t hour;
  std::uint8_t minute;

  [[nodiscard]] constexpr std::uint32_t order() const {
    return (static_cast<std::uint32_t>(year) << 20) |
           (static_cast<std::uint32_t>(month) << 16) |
           (static_cast<std::uint32_t>(day) << 11) |
           (static_cast<std::uint32_t>(hour) << 6) | minute;
  }
};

// Parses "thing=<id>/year=YYYY/month=MM/day=DD/data_HHMM_<boot>_<a>-<b>-<c>
// .parquet". Partials, unsynced, quarantine, bench and any other name give
// nullopt, so they are never uploaded.
[[nodiscard]] std::optional<FileTime> finalized_dated(std::string_view name);

class LedgerView {
public:
  [[nodiscard]] virtual bool contains(std::string_view name) const = 0;

protected:
  ~LedgerView() = default;
};

// Picks the oldest eligible file from an archive listing, one offer at a
// time, in O(1) memory. The ledger is asked only about a better candidate.
class NextFile {
public:
  explicit NextFile(const LedgerView &ledger) : ledger_(ledger) {}

  void offer(std::string_view candidate, std::uint64_t file_bytes);

  [[nodiscard]] bool found() const { return size_ > 0; }
  [[nodiscard]] std::string_view name() const { return {name_.data(), size_}; }
  [[nodiscard]] std::uint64_t bytes() const { return bytes_; }

private:
  const LedgerView &ledger_;
  std::array<char, kMaxArchiveName> name_{};
  std::size_t size_ = 0;
  std::uint64_t bytes_ = 0;
  std::uint32_t order_ = 0;
};

// Mirrors utc::Source so this header needs no logger include.
enum class ClockSource : std::uint8_t { None, Host, Rtc, Network };

// Host and network time only. The RTC can drift or hold a stale battery
// backed value, so it does not count yet (#81 owner note).
[[nodiscard]] constexpr bool utc_trusted(ClockSource source) {
  return source == ClockSource::Host || source == ClockSource::Network;
}

inline constexpr std::uint32_t kBackoffFirstMs = 30000;
inline constexpr std::uint32_t kBackoffMaxMs = 3600000;

struct PolicyState {
  std::uint32_t failures = 0;
  std::uint64_t retry_at_ms = 0;
  bool stopped = false;
  std::uint32_t stopped_key_generation = 0;
};

struct Inputs {
  bool enabled = false;
  ClockSource clock = ClockSource::None;
  bool network_up = false;
  std::uint64_t mono_ms = 0;
  // Bumped whenever a new API key is provisioned.
  std::uint32_t key_generation = 0;
};

enum class Gate : std::uint8_t {
  Disabled,
  Stopped,
  ClockUntrusted,
  Offline,
  BackingOff,
  Run,
};

[[nodiscard]] Gate decide(const PolicyState &state, const Inputs &in);

enum class Outcome : std::uint8_t {
  Uploaded,
  AlreadyUploaded,
  NothingPending,
  Transient,
  AuthRejected,
  RequestRejected,
  OutsideTarget,
};

[[nodiscard]] std::uint32_t backoff_ms(std::uint32_t failures);

// Folds one step's outcome into the state.
void apply(PolicyState &state, Outcome outcome, const Inputs &in);

enum class PutClass : std::uint8_t {
  Created,
  Exists,
  // 403 ExpiredToken, refresh the credentials and retry later.
  Expired,
  Auth,
  Transient,
  Rejected,
};

// `status` 0 means the transport failed before a response.
[[nodiscard]] PutClass classify_put(int status, std::string_view error_code);
// A failed STS exchange. Any 4xx but 408 and 429, including
// InvalidIdentityToken, stops the uploader. Everything else, including a 2xx
// body that did not parse, is transient.
[[nodiscard]] Outcome classify_sts_failure(int status);

} // namespace opensensor::upload
