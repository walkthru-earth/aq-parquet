// Board neutral footer, cadence and BLE name checks. A non air quality
// fixture board (no PMS, 30 s period, SL- prefix) writes a real Parquet file
// through the shared writer, and tools/test_logger_footer.py reads it with
// PyArrow and DuckDB. The default footer bytes the phone reads are pinned.
#include "aq_logger_footer.h"
#include <ble_advert.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <source_location>
#include <span>

namespace {
using aqlogger::Field;
using telemetry::KeyValue;
using telemetry::PhysicalType;
namespace cadence = aqlogger::cadence;
namespace footer = aqlogger::footer;

// Side effects stay in the condition, so this never compiles out.
void check(bool condition,
           std::source_location where = std::source_location::current()) {
  if (condition)
    return;
  std::fprintf(stderr, "check failed at line %u\n", unsigned(where.line()));
  std::abort();
}

void same(const char *actual, const char *expected) {
  check(actual && expected && std::strcmp(actual, expected) == 0);
}

constexpr Field kRuntimeFields[] = {
    {"sequence", PhysicalType::Int64, "runtime", "count", "always", "urn:seq"},
    {"monotonic_us", PhysicalType::Int64, "runtime", "us", "always",
     "urn:mono"},
    {"event_time_utc_us", PhysicalType::Int64, "runtime", "us_since_unix_epoch",
     "null before a clock anchor", "urn:utc"},
    {"clock_epoch", PhysicalType::Int32, "runtime", "count", "always",
     "urn:epoch"},
};

aqlogger::Config base_config() {
  return {
      .fields = kRuntimeFields,
      .column_count = std::size(kRuntimeFields),
      .schema_version = 1,
      .schema_name = "schema",
      .firmware = "firmware",
      .dictionary_version = "dictionary",
      .dictionary_uri = "uri",
      .dictionary_sha256 = "sha",
      .configuration_id = "config-id",
      .configuration = "{}",
      .time_semantics = "time",
      .board = "board",
  };
}

// Boards that set none of the new fields keep the exact keys, order and
// default values (10000, 90, unknown) that the phone reads per file.
void released_defaults() {
  const auto config = base_config();
  check(config.sample_interval_ms == 10000);
  check(config.ble_name_prefix == nullptr); // required board constant
  std::array<KeyValue, footer::kFixedEntries> out{};
  footer::Text text;
  footer::fill(out, config, {.device = "d", .station = "s", .boot = "b"},
               {.rotation_seconds = 900,
                .row_groups = 1,
                .compression = "LZ4_RAW",
                .benchmark = false},
               text);
  constexpr std::array<std::array<const char *, 2>, footer::kFixedEntries>
      kFrozen = {{
          {"schema_version", "schema"},
          {"device_id", "d"},
          {"station_id", "s"},
          {"boot_id", "b"},
          {"firmware", "firmware"},
          {"dictionary_version", "dictionary"},
          {"dictionary_uri", "uri"},
          {"dictionary_sha256", "sha"},
          {"acquisition_config_id", "config-id"},
          {"acquisition_config", "{}"},
          {"deployment_id", "unknown"},
          {"calibration_id", "unknown"},
          {"time_semantics", "time"},
          {"rotation_interval_s", "900"},
          {"row_groups", "1"},
          {"row_group_rows_max", "90"},
          {"compression", "LZ4_RAW"},
          {"purpose", "telemetry"},
          {"board", "board"},
          {"sample_interval_ms", "10000"},
          {"clock",
           "0=unsynchronized/null,1=host estimate,2=restored from RTC "
           "(earlier external estimate, whole seconds),3=network SNTP; "
           "partitions UTC"},
          {"durability",
           "RAM batch; unfinished rows lost on reset; each row group "
           "fsynced; footer at finalization; completed files retained"},
      }};
  for (std::size_t i = 0; i < kFrozen.size(); ++i) {
    same(out[i].key, kFrozen[i][0]);
    same(out[i].value, kFrozen[i][1]);
  }
  footer::fill(out, config, {.device = "d", .station = "s", .boot = "b"},
               {.rotation_seconds = 3600,
                .row_groups = 4,
                .compression = "UNCOMPRESSED",
                .benchmark = true},
               text);
  same(out[13].value, "3600");
  same(out[14].value, "4");
  same(out[16].value, "UNCOMPRESSED");
  same(out[17].value, "codec-comparison-duplicate-rows");
  // The old rule was rotation / 10, capped at 90 rows.
  for (const std::uint32_t rotation : {600U, 900U, 1800U, 3600U}) {
    const std::size_t old_rows = rotation / 10 < 90 ? rotation / 10 : 90;
    check(cadence::rows_per_group(rotation, 10000) == old_rows);
  }
}

void cadence_bounds() {
  for (const std::uint32_t ms : {1000U, 5000U, 10000U, 30000U, 60000U, 300000U})
    check(cadence::valid_sample_interval(ms));
  for (const std::uint32_t ms : {0U, 999U, 7000U, 45000U, 600000U})
    check(!cadence::valid_sample_interval(ms));
  check(cadence::rows_per_group(600, 1000) == 90);
  check(cadence::rows_per_group(600, 300000) == 2);
  check(cadence::rows_per_group(900, 30000) == 30);
  check(cadence::rows_per_group(3600, 30000) == 90);
  check(cadence::rows_per_group(0, 10000) == 1);
  check(cadence::rows_per_group(900, 0) == 1);
}

void names() {
  std::array<char, 16> name{};
  std::array<char, 24> host{};
  const ble::Identity unset{"s", "0123456789ab", "b", "schema", 0, "sha", "fw"};
  check(unset.name_prefix == nullptr);
  check(!ble::wire::local_name(name, unset.name_prefix, unset.device));
  // CoreS3 and Waveshare pass the OpenSensor prefix.
  check(ble::wire::local_name(name, "OS-", "0123456789ab"));
  same(name.data(), "OS-89ab");
  check(ble::wire::mdns_host(host, name.data()));
  same(host.data(), "os-89ab");
  check(ble::wire::local_name(name, "SL-", "0123456789ab"));
  same(name.data(), "SL-89ab");
  check(ble::wire::mdns_host(host, name.data()));
  same(host.data(), "sl-89ab");
  // Seven prefix characters reach the 11 character scan response limit.
  check(ble::wire::local_name(name, "ABCDEFG", "6b40"));
  same(name.data(), "ABCDEFG6b40");
  ble::wire::ScanResponse response;
  check(ble::wire::legacy_scan_response(name.data(), response));
  check(!ble::wire::legacy_scan_response("ABCDEFGH6b40", response));
  // The prefix validator itself, at and past the seven character limit.
  static_assert(ble::wire::valid_name_prefix("OS-"));
  static_assert(ble::wire::valid_name_prefix("ABCDEFG"));
  static_assert(!ble::wire::valid_name_prefix("ABCDEFGH"));
  check(ble::wire::valid_name_prefix("ABCDEFG"));
  check(!ble::wire::valid_name_prefix("ABCDEFGH"));
  check(!ble::wire::valid_name_prefix(""));
  check(!ble::wire::valid_name_prefix(nullptr));
  for (const char *bad : std::array<const char *, 6>{"", "ABCDEFGH", "OS_",
                                                     "OS ", "os.", nullptr})
    check(!ble::wire::local_name(name, bad, "0123456789ab") && name[0] == '\0');
  std::array<char, 7> small{}; // OS-89ab needs 8 with its NUL
  check(!ble::wire::local_name(small, "OS-", "0123456789ab"));
  check(!ble::wire::local_name(name, "OS-", nullptr));
  check(!ble::wire::mdns_host(host, "") && host[0] == '\0');
  check(!ble::wire::mdns_host(small, "ABCDEFG6b40") && small[0] == '\0');
}

// Non air quality fixture board, a soil probe with no PMS, 30 s period, SL-
// prefix.
constexpr Field kSoilFields[] = {
    kRuntimeFields[0],
    kRuntimeFields[1],
    kRuntimeFields[2],
    kRuntimeFields[3],
    {"soil_moisture_pct", PhysicalType::Float, "fixture capacitive probe",
     "percent", "null when the probe is absent", "urn:soil-moisture"},
    {"soil_temperature_c", PhysicalType::Float, "fixture thermistor",
     "degree_celsius", "null when the probe is absent", "urn:soil-t"},
};
constexpr KeyValue kSoilMetadata[] = {
    {"hardware_revision", "fixture soil board r1"},
};

struct SoilRow {
  std::int64_t sequence = 0, monotonic_us = 0, utc_us = 0;
  std::int32_t epoch = 0;
  float moisture = 0, temperature = 0;
  std::uint8_t utc_valid = 0, moisture_valid = 0, temperature_valid = 0;
};

bool file_sink(void *context, const std::uint8_t *bytes, std::size_t length) {
  return std::fwrite(bytes, 1, length, static_cast<FILE *>(context)) == length;
}

void soil_board(const char *path) {
  auto config = base_config();
  config.fields = kSoilFields;
  config.column_count = std::size(kSoilFields);
  config.schema_name = "soil-fixture-v1";
  config.board = "fixture soil board";
  config.metadata = kSoilMetadata;
  config.metadata_count = std::size(kSoilMetadata);
  config.sample_interval_ms = 30000;
  config.deployment_id = "fixture-plot-7";
  config.calibration_id = "soil-cal-2026-10";
  config.ble_name_prefix = "SL-";
  check(cadence::valid_sample_interval(config.sample_interval_ms));
  check(ble::wire::valid_name_prefix(config.ble_name_prefix));
  for (std::size_t i = 0; i < config.column_count; ++i)
    check(std::strncmp(config.fields[i].name, "pm", 2) != 0);

  // One 3600 s window at 30 s holds 120 rows, written as 90 + 30.
  constexpr std::uint32_t kRotation = 3600;
  const auto group_rows =
      cadence::rows_per_group(kRotation, config.sample_interval_ms);
  check(group_rows == cadence::kMaxBatchRows);
  constexpr std::size_t kRows = 120;
  static std::array<SoilRow, cadence::kMaxBatchRows> batch{};
  std::array<telemetry::Column, std::size(kSoilFields)> columns{};
  const auto stride = sizeof(SoilRow);
  const void *values[] = {&batch[0].sequence, &batch[0].monotonic_us,
                          &batch[0].utc_us,   &batch[0].epoch,
                          &batch[0].moisture, &batch[0].temperature};
  const std::uint8_t *valid[] = {nullptr,
                                 nullptr,
                                 &batch[0].utc_valid,
                                 nullptr,
                                 &batch[0].moisture_valid,
                                 &batch[0].temperature_valid};
  for (std::size_t i = 0; i < columns.size(); ++i)
    columns[i] = {
        .name = kSoilFields[i].name,
        .type = kSoilFields[i].type,
        .values = values[i],
        .stride = stride,
        .valid = valid[i],
        .valid_stride = stride,
        .logical = i == 2 ? telemetry::LogicalType::TimestampMicrosUtc
                          : telemetry::LogicalType::None,
        .field_id = static_cast<std::int32_t>(i + 1),
    };
  static telemetry::Workspace workspace{};
  FILE *file = std::fopen(path, "wb");
  check(file);
  telemetry::Writer writer;
  check(writer.begin(file_sink, file, workspace, columns.data(), columns.size())
            .ok);
  const std::int64_t sample_us = std::int64_t{config.sample_interval_ms} * 1000;
  std::size_t written = 0;
  while (written < kRows) {
    const std::size_t count =
        kRows - written < group_rows ? kRows - written : group_rows;
    for (std::size_t i = 0; i < count; ++i) {
      const auto n = static_cast<std::int64_t>(written + i);
      batch[i] = {.sequence = n,
                  .monotonic_us = 5000000 + n * sample_us,
                  .utc_us = 1790000000000000 + n * sample_us,
                  .epoch = 1,
                  .moisture = 20.0F + static_cast<float>(n % 7),
                  .temperature = 18.5F,
                  .utc_valid = 1,
                  .moisture_valid = static_cast<std::uint8_t>(n % 5 != 0),
                  .temperature_valid = 1};
    }
    check(writer.row_group(count, 1).ok);
    written += count;
  }
  constexpr std::size_t kCount = footer::kFixedEntries + 1;
  std::array<KeyValue, kCount> metadata{};
  footer::Text text;
  footer::fill(std::span<KeyValue, footer::kFixedEntries>(
                   metadata.data(), footer::kFixedEntries),
               config,
               {.device = "0123456789ab",
                .station = "fixture-station",
                .boot = "0000beef"},
               {.rotation_seconds = kRotation,
                .row_groups = writer.row_groups(),
                .compression = "UNCOMPRESSED",
                .benchmark = false},
               text);
  metadata[footer::kFixedEntries] = config.metadata[0];
  check(writer
            .finish(metadata.data(), metadata.size(), config.firmware,
                    "fixture-soil-board")
            .ok);
  check(std::fclose(file) == 0);
  std::array<char, 16> name{};
  std::array<char, 24> host{};
  check(ble::wire::local_name(name, config.ble_name_prefix, "0123456789ab"));
  check(ble::wire::mdns_host(host, name.data()));
  std::printf("%s %s\n", name.data(), host.data());
}
} // namespace

int main(int argc, char **argv) {
  check(argc == 2);
  released_defaults();
  cadence_bounds();
  names();
  soil_board(argv[1]);
}
