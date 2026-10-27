#pragma once

// Boot self test, the board neutral decision (#40).
//
// Pure. The logger feeds it the SD, sensor and radio results and the time
// since boot. Once the verdict is Healthy, the device glue
// (ossync::ota::confirm_running_app in connectivity) marks the running app
// valid with esp_ota_mark_app_valid_cancel_rollback(). Nothing ever marks an
// app invalid here.
//
// Today the bootloader is built without CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE,
// so the mark only rewrites the otadata state of the running slot to VALID.
// A board flashed by USB has blank otadata, and should_mark_valid() then skips the call.
// See docs/shared/security-hardening.md, section "OTA".

#include <cstddef>
#include <cstdint>
#include <span>

namespace opensensor::selftest {

enum class Check : std::uint8_t { Pending, Pass, Fail };

struct Inputs {
  Check sd = Check::Pending;     // archive card mounted and writable
  Check sensor = Check::Pending; // the self test field held a valid value
  Check radio = Check::Pending;  // the BLE stack started
  std::int64_t elapsed_ms = 0;   // since the logger began
  std::int64_t deadline_ms = 0;  // a check still pending then fails
};

enum class Verdict : std::uint8_t { Pending, Healthy, Unhealthy };

struct Outcome {
  Verdict verdict = Verdict::Pending;
  // "ok", or the first check that failed ("sd", "sensor", "radio"), or the
  // first one still pending at the deadline ("sd-timeout" and so on).
  const char *reason = "pending";
};

[[nodiscard]] Outcome decide(const Inputs &inputs);

// The otadata state of the running slot, esp_ota_img_states_t plus Unknown
// for a slot with no otadata entry (ESP_ERR_NOT_FOUND, a USB flashed board).
enum class OtaState : std::uint8_t {
  Unknown,
  New,
  PendingVerify,
  Valid,
  Invalid,
  Aborted,
  Undefined,
};

// Whether to call esp_ota_mark_app_valid_cancel_rollback(). Only a Healthy
// app whose state is New, PendingVerify or Undefined is marked. An Unknown
// state is skipped, since IDF answers "Running firmware is factory" and
// ESP_FAIL there. Valid needs nothing.
[[nodiscard]] bool should_mark_valid(Verdict verdict, OtaState state);

enum class Mark : std::uint8_t { Skipped, Marked, Failed };

[[nodiscard]] const char *name(Check check);
[[nodiscard]] const char *name(Verdict verdict);
[[nodiscard]] const char *name(OtaState state);
[[nodiscard]] const char *name(Mark mark);

// One NUL terminated line ending in '\n'. Holds no secrets. Returns the
// length, or 0 when `out` is too small.
inline constexpr std::size_t kLineBytes = 160;
std::size_t format_line(const Inputs &inputs, const Outcome &outcome,
                        OtaState state, Mark mark, std::span<char> out);

} // namespace opensensor::selftest
