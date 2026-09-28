#include "telemetry_contract.h"
#include <Arduino.h>
#include <SD_MMC.h>
#include <aq_logger.h>
#include <debug_log.h>
#include <esp_heap_caps.h>
#include <esp_psram.h>
#include <esp_timer.h>
#include <pms_frame.h>

#ifndef BOARD_HAS_PSRAM
#error The Waveshare logger requires the qualified 8 MB OPI PSRAM target.
#endif

namespace {
using namespace telemetry::contract;
constexpr int kSensorRx = 1;
constexpr int kSensorTx = 2;
constexpr int kSdClk = 5;
constexpr int kSdCmd = 4;
constexpr int kSdData0 = 6;
HardwareSerial sensor(1);
plantower::Parser parser{plantower::Model::Pms5003t};
PmsSnapshot latest;
bool logging = false;
bool storage_mounted = false;
constexpr int kStatusLed = 38;

void collect(aqlogger::Row &row, std::int64_t now, std::int64_t, void *) {
  PmsSnapshot snapshot = latest;
  if (snapshot.present)
    snapshot.age_ms =
        static_cast<std::uint32_t>((now - snapshot.received_mono_us) / 1000);
  apply_pms(row, snapshot, now);
  // Owner confirmed USB-only operation. Gauge ACK cannot prove battery
  // presence; battery measurements remain null until a later adapter exists.
  apply_gauge(row, {});
}

const telemetry::KeyValue kMetadata[] = {
    {"pms_status", "0=missing,1=warming,2=stale,3=sensor-error,4=valid,"
                   "5=model-mismatch"},
    {"gauge_status",
     "0=battery-not-installed-or-disabled,1=read-error,2=valid"},
    {"unavailable", "USB-only: battery not installed; no onboard external RTC; "
                    "modem/GNSS/camera not sampled"},
    {"sensor_model", "PMS5003T"},
    {"hardware_revision", "Waveshare ESP32-S3-SIM7670G-4G V2.0"},
    {"storage_bus", "SDMMC one-bit CLK5 CMD4 D0=6; no automatic format"},
};
const aqlogger::Config kLoggerConfig{
    .fields = kFields,
    .column_count = field_count,
    .schema_version = kSchemaVersion,
    .schema_name = kSchemaName,
    .firmware = kFirmware,
    .created_by = kCreatedBy,
    .dictionary_version = kDictionaryVersion,
    .dictionary_uri = kDictionaryUri,
    .dictionary_sha256 = kDictionarySha256,
    .configuration_id = kConfigurationId,
    .configuration = kConfiguration,
    .time_semantics = kTimeSemantics,
    .board = "Waveshare ESP32-S3-SIM7670G-4G V2.0 ESP32-S3 rev0.2",
    .output_directory = "/sd/output",
    .metadata = kMetadata,
    .metadata_count = sizeof(kMetadata) / sizeof(kMetadata[0]),
};
const aqlogger::Hooks kHooks{
    .collect = collect,
    .storage_total_bytes = [](void *) { return SD_MMC.totalBytes(); },
    .storage_used_bytes = [](void *) { return SD_MMC.usedBytes(); },
};
} // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  rgbLedWrite(kStatusLed, 0, 0, 8);
  aqlog.printf("AQ BOARD waveshare-sim7670g-v2 firmware=%s sensor=PMS5003T "
               "power=usb battery=absent display=absent rtc=absent\n",
               kFirmware);
  const bool psram_ok =
      esp_psram_is_initialized() && esp_psram_get_size() == 8 * 1024 * 1024;
  aqlog.printf("AQ MEMORY flash_bytes=%lu psram_physical_bytes=%lu "
               "psram_heap_bytes=%lu psram_ok=%u\n",
               static_cast<unsigned long>(ESP.getFlashChipSize()),
               static_cast<unsigned long>(esp_psram_get_size()),
               static_cast<unsigned long>(ESP.getPsramSize()), psram_ok);
  if (!psram_ok) {
    aqlog.println("AQ ERROR operation=psram logging=false");
    rgbLedWrite(kStatusLed, 8, 0, 0);
    return;
  }
  sensor.begin(9600, SERIAL_8N1, kSensorRx, kSensorTx);
  const bool pins_ok = SD_MMC.setPins(kSdClk, kSdCmd, kSdData0);
  const bool mounted = pins_ok && SD_MMC.begin("/sd", true, false);
  storage_mounted = mounted;
  aqlog.printf(
      "AQ TF pins_ok=%u mounted=%u card_type=%u size_bytes=%llu\n", pins_ok,
      mounted, mounted ? static_cast<unsigned>(SD_MMC.cardType()) : 0U,
      mounted ? static_cast<unsigned long long>(SD_MMC.cardSize()) : 0ULL);
  logging = aqlogger::begin(kLoggerConfig, kHooks, mounted);
  if (logging && !aqlogger::start_links(false))
    aqlog.println("AQ ERROR operation=sync-start");
  aqlog.printf("AQ READY logging=%u mounted=%u sample_ms=10000 "
               "pms_rx=%d pms_tx=%d\n",
               logging, mounted, kSensorRx, kSensorTx);
}

void loop() {
  // UART is owned by this board adapter. Worker callbacks never access it.
  for (unsigned limit = 0; limit < 256 && sensor.available(); ++limit) {
    plantower::Frame frame;
    if (parser.push(static_cast<std::uint8_t>(sensor.read()), frame)) {
      latest.frame = frame;
      latest.received_mono_us = esp_timer_get_time();
      ++latest.frames;
      latest.present = true;
    }
  }
  latest.checksum_errors = parser.checksum_failures();
  latest.length_errors = parser.length_failures();
  if (logging)
    aqlogger::poll();
  // Board-local indicator only: startup/card state and latest sensor validity.
  // It does not promise that a file was finalized or copied to another device.
  const auto now = esp_timer_get_time();
  const bool fresh =
      latest.present && now - latest.received_mono_us <= 5000000LL;
  const unsigned state =
      !logging || !storage_mounted                                ? 0
      : !fresh || now < kPmsWarmupUs || latest.frame.sensor_error ? 1
                                                                  : 2;
  static unsigned previous_state = 3;
  if (state != previous_state) {
    rgbLedWrite(kStatusLed, state == 2 ? 0 : 8, state == 0 ? 0 : 8, 0);
    previous_state = state;
  }
  delay(2);
}
