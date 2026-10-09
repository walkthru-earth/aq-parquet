#include <aq_logger.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static constexpr aqlogger::Field fields[] = {
    {"sequence", telemetry::PhysicalType::Int64, "runtime", "count", "always",
     "urn:test:sequence"},
    {"monotonic_us", telemetry::PhysicalType::Int64, "runtime", "us", "always",
     "urn:test:monotonic"},
    {"event_time_utc_ns", telemetry::PhysicalType::Int64, "runtime",
     "ns_since_unix_epoch", "clock", "urn:test:utc"},
    {"clock_epoch", telemetry::PhysicalType::Int32, "runtime", "count",
     "always", "urn:test:epoch"}};
extern "C" void app_main() {
  aqlogger::Config config;
  config.fields = fields;
  config.column_count = sizeof(fields) / sizeof(fields[0]);
  config.schema_version = 1;
  config.schema_name = "compile-fixture-v1";
  config.firmware = "aqlogger-compile-fixture";
  config.dictionary_version = "compile-fixture-v1";
  config.dictionary_uri = "urn:test:dictionary";
  config.dictionary_sha256 = "test";
  config.configuration_id = "test";
  config.configuration = "{}";
  config.time_semantics = "fixture";
  config.board = "generic-esp32-s3-compile-only";
  aqlogger::Hooks hooks;
  aqlogger::begin(config, hooks, false);
  // Headless startup compiles the real fixed-mode security callback path.
  // This fixture does not execute pairing or establish a bond.
  aqlogger::start_links(false);
  for (;;) {
    aqlogger::poll();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
