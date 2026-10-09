#include "../firmware/esp-idf-waveshare-sim7670g/logger/telemetry_contract.h"
#include <lz4_codec.h>

#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>

using namespace telemetry;
using namespace telemetry::contract;

namespace {
bool sink(void *context, const std::uint8_t *bytes, std::size_t count) {
  return std::fwrite(bytes, 1, count, static_cast<FILE *>(context)) == count;
}
std::array<std::uint8_t, 32> sensor_bytes() {
  std::array<std::uint8_t, 32> bytes{};
  bytes[0] = 0x42;
  bytes[1] = 0x4d;
  bytes[3] = 28;
  for (std::size_t i = 0; i < 10; ++i)
    bytes[5 + 2 * i] = static_cast<std::uint8_t>(i + 1);
  bytes[24] = 0xff;
  bytes[25] = 0xdb; // signed -37 deciC
  bytes[26] = 2;
  bytes[27] = 122; // 63.4% RH
  bytes[28] = 7;
  std::uint16_t sum = 0;
  for (std::size_t i = 0; i < 30; ++i)
    sum += bytes[i];
  bytes[30] = static_cast<std::uint8_t>(sum >> 8);
  bytes[31] = static_cast<std::uint8_t>(sum);
  return bytes;
}
PmsSnapshot parsed_snapshot() {
  plantower::Parser parser{plantower::Model::Pms5003t};
  PmsSnapshot pms{};
  for (const auto byte : sensor_bytes())
    pms.present = parser.push(byte, pms.frame);
  assert(pms.present && pms.frame.particle_count_fields == 4);
  assert(pms.frame.temperature_deci_c == -37);
  assert(pms.frame.humidity_deci_percent == 634);
  pms.frames = 1;
  pms.age_ms = 250;
  pms.checksum_errors = 2;
  pms.length_errors = 3;
  return pms;
}
void assert_gates() {
  auto pms = parsed_snapshot();
  Sample warm{}, ready{}, stale{}, bad_model{}, bad_gauge{}, installed{};
  apply_pms(warm, pms, kPmsWarmupUs - 1);
  assert(warm.data[pms_status] == kPmsWarming &&
         !warm.valid[ambient_temperature_c]);
  pms.age_ms = kPmsStaleAfterMs;
  apply_pms(ready, pms, kPmsWarmupUs);
  assert(ready.data[pms_status] == kPmsValid && ready.valid[pm1_cf1_ug_m3]);
  const SensorProfile exact{"Plantower", "PMS5003T", "PMS5003T-202604081332",
                            true};
  Sample candidate{};
  apply_pms(candidate, pms, kPmsWarmupUs, exact);
  assert(candidate.data[pm25_batch_candidate_status] == kBatchCandidate &&
         candidate.valid[pm25_batch_candidate_ug_m3] &&
         candidate.data[sensor_serial_code] == 202604081332LL &&
         candidate.data[pm25_atmospheric_ug_m3] ==
             ready.data[pm25_atmospheric_ug_m3]);
  Sample longer_serial{};
  apply_pms(longer_serial, pms, kPmsWarmupUs,
            {"Plantower", "PMS5003T", "PMS5003T-2026040813327", true});
  assert(longer_serial.data[pm25_batch_candidate_status] == kBatchCandidate &&
         longer_serial.data[sensor_serial_code] == 2026040813327LL);
  Sample other_batch{};
  apply_pms(other_batch, pms, kPmsWarmupUs,
            {"Plantower", "PMS5003T", "PMS5003T-202605011332", true});
  assert(other_batch.data[pm25_batch_candidate_status] ==
             kBatchIdentityMismatch &&
         !other_batch.valid[pm25_batch_candidate_ug_m3]);
  Sample wrong_vendor{};
  apply_pms(wrong_vendor, pms, kPmsWarmupUs,
            {"Other", "PMS5003T", "PMS5003T-202604081332", true});
  assert(wrong_vendor.data[pm25_batch_candidate_status] ==
             kBatchIdentityMismatch &&
         !wrong_vendor.valid[sensor_serial_code]);
  Sample unavailable{};
  apply_pms(unavailable, pms, kPmsWarmupUs - 1, exact);
  assert(unavailable.data[pm25_batch_candidate_status] ==
             kBatchSourceUnavailable &&
         !unavailable.valid[pm25_batch_candidate_ug_m3]);
  ++pms.age_ms;
  apply_pms(stale, pms, kPmsWarmupUs);
  assert(stale.data[pms_status] == kPmsStale && !stale.valid[pm1_cf1_ug_m3]);
  pms.age_ms = 250;
  plantower::Parser wrong{plantower::Model::Pmsa003};
  for (const auto byte : sensor_bytes())
    wrong.push(byte, pms.frame);
  apply_pms(bad_model, pms, kPmsWarmupUs);
  assert(bad_model.data[pms_status] == kPmsModelMismatch);
  assert(!bad_model.valid[ambient_temperature_c]);
  apply_gauge(bad_gauge, {true, false, 4200, 100});
  assert(bad_gauge.data[gauge_status] == 1 && !bad_gauge.valid[battery_mv]);
  const auto decoded = decode_max17048(0xd200, 0x6400);
  assert(decoded.read_ok && decoded.millivolts == 4200 &&
         decoded.percent == 100);
  apply_gauge(installed, decoded);
  assert(installed.valid[battery_mv] && installed.valid[battery_percent]);
  assert(!decode_max17048(0, 0x5000).read_ok);
  const auto over_full = decode_max17048(0xd5c0, 0x6bc9);
  assert(over_full.read_ok && over_full.millivolts == 4275 &&
         over_full.percent == 100);
  assert(!decode_max17048(0xd200, 0xffff).read_ok);
  Sample earlier{}, corrected{};
  apply_clock(earlier, 30000000, 15000000, 1788890000000000000LL, 1);
  const auto utc = earlier.data[event_time_utc_ns];
  apply_clock(corrected, 40000000, 35000000, 1788889000000000000LL, 2);
  assert(corrected.data[clock_epoch] == 2);
  assert(corrected.data[event_time_utc_ns] == 1788889005000000000LL);
  assert(earlier.data[event_time_utc_ns] == utc);
  Sample network{};
  apply_clock(network, 40000000, 35000000, 1788889000123456000LL, 3,
              kClockNetwork);
  assert(network.data[clock_status] == kClockNetwork);
  assert(network.valid[event_time_utc_ns] &&
         network.data[event_time_utc_ns] == 1788889005123456000LL);
  assert(network.valid[clock_anchor_mono_us] &&
         network.valid[clock_anchor_utc_ns]);
  Sample unsynced{};
  apply_clock(unsynced, 30000000, 0, 0, 0, kClockNetwork);
  assert(unsynced.data[clock_status] == kClockNone);
  assert(!unsynced.valid[event_time_utc_ns]);
}
} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::strcmp(argv[1], "dictionary") == 0) {
    std::printf("{\"schema\":\"%s\",\"schema_version\":%d,"
                "\"dictionary\":\"%s\",\"firmware\":\"%s\",\"sha256\":\"%s\","
                "\"uri\":\"%s\",\"fields\":[",
                kSchemaName, int(kSchemaVersion), kDictionaryVersion, kFirmware,
                kDictionarySha256, kDictionaryUri);
    for (std::size_t i = 0; i < field_count; ++i) {
      const auto &field = kFields[i];
      std::printf("%s{\"name\":\"%s\",\"type\":%u,\"procedure\":\"%s\","
                  "\"unit\":\"%s\",\"validity\":\"%s\",\"property\":\"%s\"}",
                  i ? "," : "", field.name, unsigned(field.type),
                  field.procedure, field.unit, field.validity,
                  field.property_uri);
    }
    std::puts("]}");
    return 0;
  }
  if (argc != 4)
    return 2;
  assert_gates();
  const bool compressed = std::strcmp(argv[2], "lz4") == 0;
  const bool anchored = std::strcmp(argv[3], "anchored") == 0;
  Sample rows[90]{};
  Column columns[field_count]{};
  Workspace workspace{};
  Lz4Workspace lz4{};
  const auto compression = lz4.configuration();
  for (std::size_t i = 0; i < 90; ++i) {
    auto &row = rows[i];
    const auto now = std::int64_t(10000000 + i * 10000000);
    row.integer(schema_version, kSchemaVersion);
    row.counter(device_id, 123);
    row.counter(boot_id_hi, 456);
    row.counter(boot_id_lo, 789);
    row.counter(sequence, i);
    row.counter(monotonic_us, now);
    row.counter(scheduled_us, now - 123);
    row.counter(sample_jitter_us, 123);
    apply_clock(row, now, 15000000, 1788890000000000000LL, anchored ? 1 : 0);
    row.counter(collection_completed_mono_us, now + 1234);
    auto pms = parsed_snapshot();
    pms.received_mono_us = now - 250000;
    pms.present = i != 0;
    if (i == 2)
      pms.age_ms = 5001;
    if (i == 3)
      pms.frame.sensor_error = 9;
    if (i == 4)
      pms.frame.particle_count_fields = 6;
    if (i == 6)
      pms.frame.humidity_deci_percent = 1001;
    apply_pms(row, pms, now,
              {"Plantower", "PMS5003T", "PMS5003T-202604081332", i != 5});
    if (i == 0)
      apply_gauge(row, {}); // explicit no-battery configuration
    else if (i == 1)
      apply_gauge(row, {true}); // failed I2C transaction
    else
      apply_gauge(row, decode_max17048(0xc800, 0x4b80));
  }
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
  Writer writer;
  auto result = writer.begin(sink, file, workspace, columns, field_count,
                             compressed ? &compression : nullptr);
  if (result.ok)
    result = writer.row_group(90, sequence);
  if (result.ok)
    result = writer.finish(metadata, sizeof(metadata) / sizeof(metadata[0]),
                           kFirmware, kCreatedBy);
  const bool closed = std::fclose(file) == 0;
  return result.ok && closed ? 0 : 4;
}
