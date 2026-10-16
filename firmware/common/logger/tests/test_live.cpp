#include <aq_logger_live.h>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

using aqlogger::Field;
using telemetry::PhysicalType;

namespace {
Field field(const char *name, PhysicalType type, const char *live_key) {
  Field result;
  result.name = name;
  result.type = type;
  result.procedure = "fixture";
  result.unit = "1";
  result.validity = "always";
  result.property_uri = "urn:fixture";
  result.live_key = live_key;
  return result;
}

void keys() {
  const Field good[] = {field("a", PhysicalType::Int64, "seq"),
                        field("b", PhysicalType::Int32, nullptr),
                        field("c", PhysicalType::Float, "imu_t"),
                        field("d", PhysicalType::Int32, "n10")};
  assert(aqlogger::valid_live_keys(good, 4));
  assert(aqlogger::valid_live_keys(nullptr, 0));
  const Field duplicate[] = {field("a", PhysicalType::Int64, "t"),
                             field("b", PhysicalType::Float, "t")};
  assert(!aqlogger::valid_live_keys(duplicate, 2));
  for (const char *bad : {"", "T", "a-b", "a\"b", "sixteen_chars_xx"}) {
    const Field one[] = {field("a", PhysicalType::Int32, bad)};
    assert(!aqlogger::valid_live_keys(one, 1));
  }
  const Field longest[] = {field("a", PhysicalType::Int32, "fifteen_chars_x")};
  assert(aqlogger::valid_live_keys(longest, 1));
}

// Prints each document for the runner to parse with a real JSON reader.
void documents() {
  const Field fields[] = {
      field("sequence", PhysicalType::Int64, "seq"),
      field("file_only", PhysicalType::Int32, nullptr),
      field("pm25", PhysicalType::Int32, "pm25"),
      field("temperature", PhysicalType::Float, "t"),
      field("pm1", PhysicalType::Int32, "pm1"),
      field("utc", PhysicalType::Int64, "utc"),
      field("humidity", PhysicalType::Float, "rh"),
  };
  aqlogger::Row row{};
  row.set(0, std::int64_t{42});
  row.set(1, std::int32_t{7}); // valid but has no key
  row.set(2, std::int32_t{-12});
  row.set(3, 26.06f);
  row.set(4, std::numeric_limits<std::int32_t>::min());
  row.set(6, std::numeric_limits<float>::quiet_NaN()); // never "nan"
  char json[256];
  const auto length = aqlogger::format_live(fields, 7, row, json, sizeof(json));
  assert(length == std::strlen(json));
  assert(std::strcmp(json, "{\"seq\":42,\"pm25\":-12,\"t\":26.1,"
                           "\"pm1\":-2147483648}") == 0);
  std::puts(json);
  // Exactly enough room for the NUL succeeds, one byte less is refused.
  char exact[64];
  assert(aqlogger::format_live(fields, 7, row, exact, length + 1) == length);
  assert(aqlogger::format_live(fields, 7, row, exact, length) == 0);
  assert(exact[0] == '\0');
  row.set(5, std::numeric_limits<std::int64_t>::max());
  row.set(6, -0.04f);
  assert(aqlogger::format_live(fields, 7, row, json, sizeof(json)));
  std::puts(json);
  const aqlogger::Row empty{};
  assert(aqlogger::format_live(fields, 7, empty, json, sizeof(json)) == 2);
  assert(std::strcmp(json, "{}") == 0);
  std::puts(json);
  assert(aqlogger::format_live(fields, 7, row, json, 0) == 0);
  assert(aqlogger::format_live(fields, 7, row, nullptr, 8) == 0);
}

// Rows store UTC in microseconds, LIVE `utc` stays in nanoseconds.
void utc_scale() {
  Field utc = field("event_time_utc_us", PhysicalType::Int64, "utc");
  utc.unit = "us_since_unix_epoch";
  aqlogger::Row row{};
  row.set(0, std::int64_t{1757990410000123});
  char json[64];
  assert(aqlogger::format_live(&utc, 1, row, json, sizeof(json)));
  assert(std::strcmp(json, "{\"utc\":1757990410000123000}") == 0);
  row.set(0, std::numeric_limits<std::int64_t>::max() / 1000 + 1);
  assert(aqlogger::format_live(&utc, 1, row, json, sizeof(json)) == 2);
  row.set(0, std::numeric_limits<std::int64_t>::min() / 1000 - 1);
  assert(aqlogger::format_live(&utc, 1, row, json, sizeof(json)) == 2);
}
} // namespace

int main() {
  keys();
  documents();
  utc_scale();
  return 0;
}
