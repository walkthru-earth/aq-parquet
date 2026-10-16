#include "../board_io.h"
#include "telemetry_contract.h"
#include <aq_console.h>
#include <aq_logger.h>
#include <aq_max17048.h>
#include <debug_log.h>
#include <device_config.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_psram.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <log_persist.h>
#include <nvs_flash.h>
#include <pms_frame.h>
#include <power.h>

#ifndef CONFIG_SPIRAM
#error The Waveshare logger requires the qualified 8 MB OPI PSRAM target.
#endif

namespace {
using namespace telemetry::contract;
constexpr int kSensorRx = 1;
constexpr int kSensorTx = 2;
constexpr int kSdClk = 5;
constexpr int kSdCmd = 4;
constexpr int kSdData0 = 6;
constexpr int kGaugeSda = 15;
constexpr int kGaugeScl = 16;
// The owner installed an 18650 and verified battery-only operation. A gauge
// ACK alone cannot prove presence; set false if this image is used without it.
constexpr bool kBatteryInstalled = true;
// The wired PMS5003T decides which provisioned identities the runtime keeps.
constexpr config::SensorPolicy kSensorPolicy{
    pms5003t::kVendor, pms5003t::kModel, pms5003t::valid_serial,
    pms5003t::batch_eligible};

plantower::Parser parser{plantower::Model::Pms5003t};
PmsSnapshot latest;
bool logging = false;
bool storage_mounted = false;
bool gauge_bus_ready = false;
const aq::Max17048 gauge{{nullptr, board::gauge_read, board::gauge_write}};
aq::battery::Monitor battery_monitor{kBatteryThresholds};

// Main loop only. Reads VCELL/SOC, then the diagnostic registers, feeds the
// shared classifier and reports the state to the logger runtime.
BatteryHealth read_gauge(std::int64_t now) {
  BatteryHealth health;
  if (!kBatteryInstalled)
    return health;
  health.battery_installed = true;
  if (gauge_bus_ready) {
    health.reading = gauge.read();
    health.extended = gauge.read_extended();
  } else {
    health.reading.io_error = true;
  }
  health.assessment = battery_monitor.update(
      battery_sample(now / 1000, health.reading, health.extended));
  static unsigned diagnostics = 0;
  if (gauge_bus_ready && diagnostics++ < 6)
    aqlog.printf(
        "AQ GAUGE read io_error=%u error=%d vcell_raw=%u soc_raw=%u "
        "plausible=%u crate_raw=%d status=0x%04x mode=0x%04x\n",
        health.reading.io_error, board::gauge_last_error(),
        health.reading.raw_vcell, health.reading.raw_soc,
        health.reading.plausible,
        static_cast<int>(static_cast<std::int16_t>(health.extended.crate.raw)),
        health.extended.status.raw, health.extended.mode.raw);
  aqlogger::report_battery(health.assessment.state,
                           health.reading.valid() ? health.reading.millivolts
                                                  : -1);
  return health;
}

// Boot only, before logging starts. A set reset indicator means the gauge
// lost power or reset, so its SOC needs the settling window. Clear it once
// so a later reset is visible again.
void check_gauge_reset() {
  const auto ext = gauge.read_extended();
  const bool ri = ext.status_bit(aq::Max17048Extended::kStatusRi);
  aqlog.printf("AQ GAUGE status=0x%04x status_ok=%u ri=%u version=0x%04x "
               "hibrt=0x%04x config=0x%04x\n",
               ext.status.raw, ext.status.ok, ri, ext.version.raw,
               ext.hibrt.raw, ext.config.raw);
  if (!ri)
    return;
  battery_monitor.note_gauge_reset(esp_timer_get_time() / 1000);
  const auto result = gauge.clear_reset_indicator();
  aqlog.printf("AQ GAUGE ri_clear=%s error=%d\n", aq::clear_result_name(result),
               board::gauge_last_error());
}

void collect(aqlogger::Row &row, std::int64_t now, std::int64_t, void *) {
  PmsSnapshot snapshot = latest;
  if (snapshot.present)
    snapshot.age_ms =
        static_cast<std::uint32_t>((now - snapshot.received_mono_us) / 1000);
  const auto settings = config::get();
  apply_pms(row, snapshot, now,
            {settings.sensor_vendor, settings.sensor_model,
             settings.sensor_serial, settings.sensor_batch_candidate});
  const auto health = read_gauge(now);
  apply_gauge(row, !health.battery_installed ? GaugeSnapshot{}
                                             : gauge_snapshot(health.reading));
  apply_battery_health(row, health);
}

const telemetry::KeyValue kMetadata[] = {
    {"pms_status", "0=missing,1=warming,2=stale,3=sensor-error,4=valid,"
                   "5=model-mismatch"},
    {"gauge_status",
     "0=battery-not-installed-or-disabled,1=read-error-or-implausible,2=valid"},
    {"unavailable",
     "charger STAT and USB VBUS are not wired to the ESP32-S3; external power "
     "is inferred from VCELL only; no battery current; no onboard external "
     "RTC; modem/GNSS/camera not sampled"},
    {"battery_health_state",
     "0=unknown,1=external-power-likely,2=ok,3=low,4=critical,5=settling; "
     "VCELL thresholds in acquisition_config are uncalibrated"},
    {"gauge_status_flags",
     "MAX17048 STATUS high byte: bit0=RI,bit1=VH,bit2=VL,bit3=VR,bit4=HD,"
     "bit5=SC,bit6=EnVr"},
    {"battery_minutes_to_empty",
     "raw SOC divided by discharge CRATE; null when charging, settling, "
     "external power likely or above 7 days; not a validated runtime"},
    {"battery_presence",
     kBatteryInstalled ? "owner-confirmed-installed" : "configured-absent"},
    {"sensor_model", "PMS5003T"},
    {"pm25_batch_candidate_formula",
     "AirGradient PMS5003T-20260408 v1: 0.003964 * particles_gt03_per_01l + 0"},
    {"pm25_batch_candidate_validation",
     "owner-opt-in research candidate; not Cairo-reference-validated; raw PM "
     "unchanged"},
    {"pm25_batch_candidate_status",
     "0=disabled,1=identity-mismatch,2=PMS-unavailable,3=candidate-only"},
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
    .storage_total_bytes = [](void *) { return board::storage_total_bytes(); },
    .storage_used_bytes = [](void *) { return board::storage_used_bytes(); },
    .storage_free_bytes =
        [](std::uint64_t &free_bytes, void *) {
          return board::storage_free_bytes(free_bytes);
        },
    .storage_cluster_bytes = [](void *) { return board::cluster_bytes(); },
    .storage_remount = [](void *) { return board::remount_card(); },
};
} // namespace

bool init_board() {
  const auto nvs_result = nvs_flash_init();
  if (nvs_result != ESP_OK) {
    aqlog.printf("AQ ERROR operation=nvs-init error=%d logging=false\n",
                 nvs_result);
    return false;
  }
  board::start_indicator();
  vTaskDelay(pdMS_TO_TICKS(500));
  board::indicator(0, 0, 8);
  aqlog.printf("AQ BOARD waveshare-sim7670g-v2 firmware=%s sensor=PMS5003T "
               "battery=%s display=absent rtc=absent\n",
               kFirmware, kBatteryInstalled ? "installed" : "absent");
  const bool psram_ok =
      esp_psram_is_initialized() && esp_psram_get_size() == 8 * 1024 * 1024;
  std::uint32_t flash_bytes = 0;
  const esp_err_t flash_result = esp_flash_get_size(nullptr, &flash_bytes);
  aqlog.printf(
      "AQ MEMORY flash_bytes=%lu flash_error=%d psram_physical_bytes=%lu "
      "psram_heap_bytes=%lu psram_ok=%u\n",
      static_cast<unsigned long>(flash_bytes), flash_result,
      static_cast<unsigned long>(esp_psram_get_size()),
      static_cast<unsigned long>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM)),
      psram_ok);
  if (!psram_ok) {
    aqlog.println("AQ ERROR operation=psram logging=false");
    board::indicator(8, 0, 0);
    return false;
  }
  if (kBatteryInstalled) {
    gauge_bus_ready = board::start_gauge();
    aqlog.printf("AQ GAUGE bus_ready=%u sda=%d scl=%d addr=0x%02x\n",
                 gauge_bus_ready, kGaugeSda, kGaugeScl, aq::Max17048::kAddress);
    if (gauge_bus_ready)
      check_gauge_reset();
    if (!battery_monitor.configured())
      aqlog.println("AQ ERROR operation=battery-thresholds reason=invalid");
  }
  const bool sensor_ready = board::start_sensor();
  const bool mounted = board::mount_card();
  storage_mounted = mounted;
  aqlog.printf("AQ TF mounted=%u size_bytes=%llu sensor_ready=%u "
               "clk=%d cmd=%d d0=%d width=1 format_attempted=false\n",
               mounted, static_cast<unsigned long long>(board::card_bytes()),
               sensor_ready, kSdClk, kSdCmd, kSdData0);
  logging = aqlogger::begin(kLoggerConfig, kHooks, mounted);
  config::set_sensor_policy(kSensorPolicy);
  if (logging && !aqlogger::start_links(false))
    aqlog.println("AQ ERROR operation=sync-start");
  aqlog.printf("AQ READY logging=%u mounted=%u sample_ms=10000 "
               "pms_rx=%d pms_tx=%d\n",
               logging, mounted, kSensorRx, kSensorTx);
  return true;
}

void poll_board() {
  // UART is owned by this board adapter. Worker callbacks never access it.
  for (unsigned limit = 0; limit < 256; ++limit) {
    const int byte = board::read_sensor();
    if (byte < 0)
      break;
    plantower::Frame frame;
    if (parser.push(static_cast<std::uint8_t>(byte), frame)) {
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
    board::indicator(state == 2 ? 0 : 8, state == 0 ? 0 : 8, 0);
    previous_state = state;
  }
  const auto ticks = pdMS_TO_TICKS(2);
  vTaskDelay(ticks ? ticks : 1);
}

namespace {
void board_task(void *) {
  if (!init_board()) {
    vTaskDelete(nullptr);
    return;
  }
  for (;;)
    poll_board();
}
} // namespace

extern "C" void app_main() {
  if (!aq::console::begin())
    return;
  aq::log::attach_persistent(); // PSRAM log; the internal ring otherwise
  aq::power::configure_dfs();
  if (xTaskCreate(board_task, "aq-board", 12288, nullptr,
                  aqlogger::kBoardTaskPriority, nullptr) != pdPASS)
    aqlog.println("AQ ERROR operation=board-task-create logging=false");
}
