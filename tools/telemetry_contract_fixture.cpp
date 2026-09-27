#include "../firmware/arduino-m5unified/bringup/telemetry_contract.h"
#include "../firmware/common/src/lz4_codec.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>

using namespace telemetry;
using namespace telemetry::contract;

namespace {
bool sink(void *context, const std::uint8_t *bytes, std::size_t count) {
  return std::fwrite(bytes, 1, count, static_cast<FILE *>(context)) == count;
}
} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::strcmp(argv[1], "dictionary") == 0) {
    std::printf("{\"schema\":\"%s\",\"schema_version\":%d,"
                "\"dictionary\":\"%s\",\"firmware\":\"%s\",\"sha256\":\"%s\","
                "\"fields\":[",
                kSchemaName, int(kSchemaVersion), kDictionaryVersion, kFirmware,
                kDictionarySha256);
    for (std::size_t i = 0; i < field_count; ++i) {
      const auto &f = kFields[i];
      // Dictionary tokens are compile-time literals, validated by the host.
      std::printf("%s{\"name\":\"%s\",\"type\":%u,\"procedure\":\"%s\","
                  "\"unit\":\"%s\",\"validity\":\"%s\",\"property\":\"%s\"}",
                  i ? "," : "", f.name, unsigned(f.type), f.procedure, f.unit,
                  f.validity, f.property_uri);
    }
    std::puts("]}");
    return 0;
  }
  if (argc != 4)
    return 2;
  const bool compressed = std::strcmp(argv[2], "lz4") == 0;
  const bool anchored = std::strcmp(argv[3], "anchored") == 0;
  Sample rows[90]{};
  Column columns[field_count]{};
  Workspace workspace{};
  Lz4Workspace lz4{};
  const auto compression = lz4.configuration();
  for (std::size_t i = 0; i < 90; ++i) {
    auto &row = rows[i];
    const auto now = std::int64_t(20000000 + i * 10000000);
    row.integer(schema_version, kSchemaVersion);
    row.counter(sequence, i);
    row.counter(monotonic_us, now);
    row.counter(scheduled_us, now - 123);
    row.counter(sample_jitter_us, 123);
    apply_clock(row, now, 15000000, 1788890000000000000LL, anchored ? 1 : 0);
    row.counter(collection_completed_mono_us, now + 1234);
    if (i > 0) {
      row.counter(pms_received_mono_us, now - 250000);
      row.counter(pms_age_ms, 250);
      row.integer(pms_status, 4);
      for (std::size_t j = 0; j < 12; ++j)
        row.integer(static_cast<Field>(pm1_cf1_ug_m3 + j), i + j);
    } else
      row.integer(pms_status, 0);
    row.number(accel_x_g, 0.125F);
    row.number(ambient_temperature_c, std::numeric_limits<float>::quiet_NaN());
    assert(!row.valid[ambient_temperature_c]);
    assert(!row.valid[battery_current_ma]);
  }
  // A new anchor must not change a previously captured row.
  Sample corrected{};
  const auto old_utc = rows[1].data[event_time_utc_ns];
  apply_clock(corrected, 30000000, 25000000, 1788889000000000000LL, 2);
  assert(corrected.data[clock_epoch] == 2);
  assert(corrected.data[event_time_utc_ns] == 1788889005000000000LL);
  assert(rows[1].data[event_time_utc_ns] == old_utc);
  // An RTC-restored anchor is dated like a host one but carries code 2; with
  // no anchor the source code is ignored and the status stays 0.
  Sample restored{}, unanchored{};
  apply_clock(restored, 30000000, 25000000, 1788889000000000000LL, 1,
              kClockRtc);
  assert(restored.data[clock_status] == kClockRtc);
  assert(restored.data[clock_epoch] == 1);
  assert(restored.data[event_time_utc_ns] == 1788889005000000000LL);
  apply_clock(unanchored, 30000000, 0, 0, 0, kClockRtc);
  assert(unanchored.data[clock_status] == kClockNone);
  assert(!unanchored.valid[event_time_utc_ns]);
  prepare_columns(columns, rows);
  const KeyValue metadata[] = {{"schema_version", kSchemaName},
                               {"firmware", kFirmware},
                               {"dictionary_version", kDictionaryVersion},
                               {"dictionary_uri", kDictionaryUri},
                               {"dictionary_sha256", kDictionarySha256},
                               {"acquisition_config_id", kConfigurationId},
                               {"acquisition_config", kConfiguration},
                               {"deployment_id", kUnknown},
                               {"calibration_id", kUnknown},
                               {"time_semantics", kTimeSemantics},
                               {"purpose", "synthetic-contract-test"}};
  FILE *file = std::fopen(argv[1], "wb");
  if (!file)
    return 3;
  // Same calls as the logger: one row group per 90-row batch, rows ascending
  // by sequence, firmware identity in created_by.
  Writer writer;
  auto result = writer.begin(sink, file, workspace, columns, field_count,
                             compressed ? &compression : nullptr);
  if (result.ok)
    result = writer.row_group(90, sequence);
  if (result.ok)
    result = writer.finish(metadata, sizeof(metadata) / sizeof(metadata[0]),
                           kFirmware);
  const bool closed = std::fclose(file) == 0;
  return result.ok && closed ? 0 : 4;
}
