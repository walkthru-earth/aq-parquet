// Bench stall command tests. The real parser and deadline from
// aq_logger_bench_stall.h, the same code the bench image runs.
#include "aq_logger_bench_stall.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace bench = aqlogger::bench;
using bench::StallError;

namespace {
void accepts() {
  static_assert(*bench::parse_stall_seconds("parquet bench-stall 0") == 0);
  static_assert(*bench::parse_stall_seconds("parquet bench-stall 64") == 64);
  static_assert(*bench::parse_stall_seconds("parquet bench-stall 90") == 90);
  static_assert(*bench::parse_stall_seconds("parquet bench-stall 3600") ==
                bench::kMaxStallSeconds);
  // Leading zeros are still decimal digits within the bound.
  static_assert(*bench::parse_stall_seconds("parquet bench-stall 0065") == 65);
  static_assert(*bench::parse_stall_seconds(
                    "parquet bench-stall 000000000000000000000000000001") == 1);
  for (std::uint32_t seconds = 0; seconds <= bench::kMaxStallSeconds;
       ++seconds) {
    const auto text = "parquet bench-stall " + std::to_string(seconds);
    const auto parsed = bench::parse_stall_seconds(text);
    assert(parsed.has_value() && *parsed == seconds);
  }
}

void refuses(std::string_view text, StallError expected) {
  const auto parsed = bench::parse_stall_seconds(text);
  assert(!parsed.has_value());
  assert(parsed.error() == expected);
}

void rejects() {
  using enum StallError;
  refuses("parquet bench-stall ", Malformed);
  refuses("parquet bench-stall", Malformed);
  refuses("parquet bench-stall  5", Malformed);
  refuses("parquet bench-stall 5 ", Malformed);
  refuses("parquet bench-stall -1", Malformed);
  refuses("parquet bench-stall +5", Malformed);
  refuses("parquet bench-stall 5s", Malformed);
  refuses("parquet bench-stall 0x10", Malformed);
  refuses("parquet bench-stall 1.5", Malformed);
  refuses("parquet bench-stallx 5", Malformed);
  refuses("parquet flush", Malformed);
  refuses("", Malformed);
  refuses(std::string_view{"parquet bench-stall 5\0", 22}, Malformed);
  refuses("parquet bench-stall 3601", OutOfRange);
  refuses("parquet bench-stall 10000", OutOfRange);
  // Far past every integer width, still bounded and not wrapped to a small
  // value.
  refuses("parquet bench-stall 4294967296", OutOfRange);
  refuses("parquet bench-stall 18446744073709551617", OutOfRange);
  refuses("parquet bench-stall 99999999999999999999999999999999", OutOfRange);
  // A digit run past the bound followed by junk is still malformed.
  refuses("parquet bench-stall 99999x", Malformed);
  static_assert(std::string_view{bench::stall_error_name(Malformed)} ==
                "invalid-seconds");
  static_assert(std::string_view{bench::stall_error_name(OutOfRange)} ==
                "seconds-out-of-range");
}

void prefix() {
  static_assert(bench::is_stall_command("parquet bench-stall 1"));
  static_assert(bench::is_stall_command("parquet bench-stall "));
  static_assert(!bench::is_stall_command("parquet bench-stall"));
  static_assert(!bench::is_stall_command("parquet bench"));
  static_assert(!bench::is_stall_command(" parquet bench-stall 1"));
}

void deadline() {
  constexpr std::int64_t start = 5'000'000;
  // A zero stall blocks forever, even at the far end of the clock.
  static_assert(bench::stall_continues(0, start, start));
  static_assert(bench::stall_continues(0, start, INT64_MAX));
  // A 64 s stall blocks until exactly 64 s have passed.
  static_assert(bench::stall_continues(64, start, start));
  static_assert(bench::stall_continues(64, start, start + 63'999'999));
  static_assert(!bench::stall_continues(64, start, start + 64'000'000));
  static_assert(!bench::stall_continues(64, start, start + 65'000'000));
  // The longest stall does not overflow the microsecond product.
  static_assert(
      bench::stall_continues(bench::kMaxStallSeconds, 0, 3'599'999'999));
  static_assert(
      !bench::stall_continues(bench::kMaxStallSeconds, 0, 3'600'000'000));
}
} // namespace

int main() {
  accepts();
  rejects();
  prefix();
  deadline();
  std::puts("bench stall parser and deadline passed");
  return 0;
}
