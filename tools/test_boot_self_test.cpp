// Host tests for the boot self test decision in
// firmware/common/src/boot_self_test.h.

#include "boot_self_test.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace st = opensensor::selftest;

namespace {
int failures = 0;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::fprintf(stderr, "%s:%d CHECK failed: %s\n", __FILE__, __LINE__,     \
                   #condition);                                                \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

using enum st::Check;

st::Inputs inputs(st::Check sd, st::Check sensor, st::Check radio,
                  std::int64_t elapsed = 1000) {
  return {.sd = sd,
          .sensor = sensor,
          .radio = radio,
          .elapsed_ms = elapsed,
          .deadline_ms = 180000};
}

bool is(const st::Outcome &outcome, st::Verdict verdict,
        std::string_view reason) {
  return outcome.verdict == verdict && reason == outcome.reason;
}

void test_decide() {
  CHECK(is(st::decide(inputs(Pass, Pass, Pass)), st::Verdict::Healthy, "ok"));
  // Every check must pass. Any one failing is unhealthy.
  CHECK(is(st::decide(inputs(Fail, Pass, Pass)), st::Verdict::Unhealthy, "sd"));
  CHECK(is(st::decide(inputs(Pass, Fail, Pass)), st::Verdict::Unhealthy,
           "sensor"));
  CHECK(is(st::decide(inputs(Pass, Pass, Fail)), st::Verdict::Unhealthy,
           "radio"));
  // A failure is final even before the deadline, and wins over pending.
  CHECK(is(st::decide(inputs(Pending, Pending, Fail)), st::Verdict::Unhealthy,
           "radio"));
  // Pending until the deadline.
  CHECK(is(st::decide(inputs(Pass, Pending, Pass)), st::Verdict::Pending,
           "pending"));
  CHECK(is(st::decide(inputs(Pass, Pending, Pass, 179999)),
           st::Verdict::Pending, "pending"));
  CHECK(is(st::decide(inputs(Pass, Pending, Pass, 180000)),
           st::Verdict::Unhealthy, "sensor-timeout"));
  CHECK(is(st::decide(inputs(Pending, Pending, Pending, 200000)),
           st::Verdict::Unhealthy, "sd-timeout"));
  CHECK(is(st::decide(inputs(Pass, Pass, Pending, 200000)),
           st::Verdict::Unhealthy, "radio-timeout"));
  // All passing is healthy even past the deadline.
  CHECK(is(st::decide(inputs(Pass, Pass, Pass, 999999)), st::Verdict::Healthy,
           "ok"));
}

void test_mark_plan() {
  using enum st::OtaState;
  // Healthy marks only a state that is not yet valid.
  CHECK(st::should_mark_valid(st::Verdict::Healthy, New));
  CHECK(st::should_mark_valid(st::Verdict::Healthy, PendingVerify));
  CHECK(st::should_mark_valid(st::Verdict::Healthy, Undefined));
  CHECK(!st::should_mark_valid(st::Verdict::Healthy, Valid));
  // A USB flashed board has no otadata entry. IDF would log an error there.
  CHECK(!st::should_mark_valid(st::Verdict::Healthy, Unknown));
  CHECK(!st::should_mark_valid(st::Verdict::Healthy, Invalid));
  CHECK(!st::should_mark_valid(st::Verdict::Healthy, Aborted));
  // Never on a pending or unhealthy verdict.
  for (const auto state :
       {Unknown, New, PendingVerify, Valid, Invalid, Aborted, Undefined}) {
    CHECK(!st::should_mark_valid(st::Verdict::Pending, state));
    CHECK(!st::should_mark_valid(st::Verdict::Unhealthy, state));
  }
}

void test_line() {
  std::array<char, st::kLineBytes> line{};
  const auto in = inputs(Pass, Pass, Pass, 42000);
  const auto length = st::format_line(
      in, st::decide(in), st::OtaState::Undefined, st::Mark::Marked, line);
  const std::string_view text(line.data(), length);
  CHECK(text == "SELFTEST result=healthy reason=ok sd=pass sensor=pass "
                "radio=pass elapsed_ms=42000 ota_state=undefined "
                "mark_valid=marked\n");
  // The longest line still fits.
  const auto worst = inputs(Pending, Pending, Pending, 9223372036854775807LL);
  CHECK(st::format_line(worst, st::decide(worst), st::OtaState::PendingVerify,
                        st::Mark::Skipped, line) > 0);
  std::array<char, 8> tiny{};
  CHECK(st::format_line(in, st::decide(in), st::OtaState::Valid,
                        st::Mark::Skipped, tiny) == 0);
  CHECK(tiny[0] == '\0');
}
} // namespace

int main() {
  test_decide();
  test_mark_plan();
  test_line();
  if (failures) {
    std::fprintf(stderr, "%d boot self test check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
