#include "telemetry_logger.h"
#include "debug_log.h"
#include "ltr553.h"
#include "telemetry_contract.h"
#include <M5Unified.h>
#include <aq_logger.h>
#include <array>
#include <ctime>
#include <esp_vfs_fat.h>
#include <utc_clock.h>

namespace telemetry {
namespace {
using namespace contract;
Ltr553 light_sensor;
PmsSnapshot latest;
std::array<aqlogger::Field, field_count> fields;

void collect(aqlogger::Row &row, std::int64_t now, std::int64_t, void *) {
  const auto &pms = latest;
  const int status = !pms.present        ? 0
                     : now < 30000000    ? 1
                     : pms.age_ms > 5000 ? 2
                     : pms.error         ? 3
                                         : 4;
  row.integer(pms_status, status);
  if (pms.present) {
    row.counter(pms_age_ms, pms.age_ms);
    row.counter(pms_received_mono_us, pms.received_mono_us);
    row.integer(pms_firmware, pms.firmware);
    row.integer(pms_error, pms.error);
  }
  if (status == 4)
    for (std::size_t i = 0; i < 12; ++i)
      row.integer(static_cast<Field>(pm1_cf1_ug_m3 + i), pms.values[i]);
  row.counter(pms_frames, pms.frames);
  row.counter(pms_checksum_errors, pms.checksum_errors);
  row.counter(pms_length_errors, pms.length_errors);
  const auto fresh =
      M5.Imu.isEnabled() ? M5.Imu.update() : m5::IMU_Class::sensor_mask_none;
  row.integer(imu_fresh_mask, fresh);
  m5::IMU_Class::imu_data_t imu{};
  M5.Imu.getImuData(&imu);
  if (fresh & m5::IMU_Class::sensor_mask_accel) {
    row.number(accel_x_g, imu.accel.x);
    row.number(accel_y_g, imu.accel.y);
    row.number(accel_z_g, imu.accel.z);
  }
  if (fresh & m5::IMU_Class::sensor_mask_gyro) {
    row.number(gyro_x_dps, imu.gyro.x);
    row.number(gyro_y_dps, imu.gyro.y);
    row.number(gyro_z_dps, imu.gyro.z);
  }
  if (fresh & m5::IMU_Class::sensor_mask_mag) {
    row.integer(mag_x_raw, M5.Imu.getRawData(6));
    row.integer(mag_y_raw, M5.Imu.getRawData(7));
    row.integer(mag_z_raw, M5.Imu.getRawData(8));
  }
  float temperature;
  if (M5.Imu.isEnabled() && M5.Imu.getTemp(&temperature))
    row.number(imu_temperature_c, temperature);
  const auto light = light_sensor.read();
  row.integer(light_status, !light_sensor.available() ? 0
                            : light.io_error          ? 4
                            : !light.als_fresh        ? 1
                            : light.als_valid         ? 3
                                                      : 2);
  row.integer(proximity_status, !light_sensor.available() ? 0
                                : light.io_error          ? 4
                                : !light.proximity_fresh  ? 1
                                : light.proximity_valid   ? 3
                                                          : 2);
  if (light.als_valid) {
    row.integer(light_ch0_raw, light.als_ch0);
    row.integer(light_ch1_raw, light.als_ch1);
  }
  if (light.proximity_valid)
    row.integer(proximity_raw, light.proximity);
  row.integer(vbus_mv, M5.Power.getVBUSVoltage());
  row.integer(battery_mv, M5.Power.getBatteryVoltage());
  const int battery = M5.Power.getBatteryLevel();
  if (battery >= 0 && battery <= 100)
    row.integer(battery_percent, battery);
  row.integer(charging_status, M5.Power.isCharging());
  // CoreS3 getBatteryCurrent() is an unsupported constant zero. Leave null.
  row.integer(external_5v_enabled, M5.Power.getExtOutput());
  row.integer(usb_output_enabled, M5.Power.getUsbOutput());
  m5::rtc_datetime_t rtc{};
  const bool rtc_ok = M5.Rtc.isEnabled() && M5.Rtc.getDateTime(&rtc);
  row.integer(rtc_read_ok, rtc_ok);
  if (rtc_ok) {
    row.integer(rtc_date_yyyymmdd,
                rtc.date.year * 10000 + rtc.date.month * 100 + rtc.date.date);
    row.integer(rtc_time_hhmmss, rtc.time.hours * 10000 +
                                     rtc.time.minutes * 100 + rtc.time.seconds);
  }
  row.integer(touch_points, M5.Touch.getCount());
  if (M5.Touch.getCount()) {
    const auto &touch = M5.Touch.getDetail();
    row.integer(touch_x_px, touch.x);
    row.integer(touch_y_px, touch.y);
  }
}

bool rtc_read(std::int64_t &seconds, void *) {
  if (!M5.Rtc.isEnabled() || M5.Rtc.getVoltLow())
    return false;
  m5::rtc_datetime_t rtc{};
  if (!M5.Rtc.getDateTime(&rtc))
    return false;
  const int year = rtc.date.year;
  const int month = rtc.date.month;
  const int day = rtc.date.date;
  if (year < 2020 || year > 2099 || month < 1 || month > 12 ||
      rtc.time.hours < 0 || rtc.time.hours > 23 || rtc.time.minutes < 0 ||
      rtc.time.minutes > 59 || rtc.time.seconds < 0 || rtc.time.seconds > 59)
    return false;
  constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const int max_day = days[month - 1] + (month == 2 && year % 4 == 0);
  if (day < 1 || day > max_day)
    return false;
  seconds =
      aq::utc::days_from_civil(year, unsigned(month), unsigned(day)) * 86400 +
      rtc.time.hours * 3600 + rtc.time.minutes * 60 + rtc.time.seconds;
  return aq::utc::supported_epoch(seconds);
}

bool rtc_write(std::int64_t seconds, void *) {
  if (!M5.Rtc.isEnabled())
    return false;
  const auto value = static_cast<std::time_t>(seconds);
  std::tm utc{};
  if (!::gmtime_r(&value, &utc))
    return false;
  M5.Rtc.setDateTime(m5::rtc_datetime_t(utc));
  std::int64_t readback = 0;
  return rtc_read(readback, nullptr) && readback >= seconds &&
         readback - seconds <= 1;
}

const KeyValue metadata[] = {
    {"pms_status", "0=missing,1=warming,2=stale,3=sensor-error,4=valid"},
    {"light_status", "0=unavailable,1=not-fresh,2=invalid,3=valid,4=io-error"},
    {"proximity_status",
     "0=unavailable,1=not-fresh,2=saturated,3=valid,4=io-error"},
    {"magnetic_units", "M5Unified BMI270 auxiliary raw counts; uncalibrated"},
    {"unavailable", "SHT20 address collision; battery current unsupported; "
                    "camera/audio not sampled"},
};
aqlogger::Config logger_config;
bool storage_mounted = false;
std::uint64_t cached_total_bytes = 0;
std::uint64_t cached_used_bytes = 0;

void refresh_capacity() {
  if (!storage_mounted)
    return;
  // Called only by the archive worker under the shared display/storage mutex.
  std::uint64_t total = 0, free = 0;
  if (esp_vfs_fat_info("/sd", &total, &free) == ESP_OK && free <= total) {
    cached_total_bytes = total;
    cached_used_bytes = total - free;
  }
}
} // namespace

void begin_logger(bool sd_mounted) {
  storage_mounted = sd_mounted;
  for (std::size_t i = 0; i < field_count; ++i) {
    const auto &field = kFields[i];
    fields[i] = {field.name, field.type,     field.procedure,
                 field.unit, field.validity, field.property_uri};
  }
  logger_config = {
      .fields = fields.data(),
      .column_count = field_count,
      .schema_version = kSchemaVersion,
      .schema_name = kSchemaName,
      .firmware = kFirmware,
      .created_by = "m5stack-aq-parquet version 0.2",
      .dictionary_version = kDictionaryVersion,
      .dictionary_uri = kDictionaryUri,
      .dictionary_sha256 = kDictionarySha256,
      .configuration_id = kConfigurationId,
      .configuration = kConfiguration,
      .time_semantics = kTimeSemantics,
      .board = "CoreS3 ESP32-S3 rev0.2",
      .output_directory = "/sd/output",
      .legacy_directory = "/sd/parquet",
      .metadata = metadata,
      .metadata_count = sizeof(metadata) / sizeof(metadata[0]),
  };
  const aqlogger::Hooks hooks{
      .collect = collect,
      .storage_total_bytes =
          [](void *) {
            refresh_capacity();
            return cached_total_bytes;
          },
      .storage_used_bytes =
          [](void *) {
            refresh_capacity();
            return cached_used_bytes;
          },
      .rtc_read = rtc_read,
      .rtc_write = rtc_write,
  };
  const bool light = light_sensor.begin();
  aqlog.printf("PARQUET HARDWARE board=cores3 light_available=%s\n",
               light ? "true" : "false");
  aqlogger::begin(logger_config, hooks, sd_mounted);
}
void poll_logger(const PmsSnapshot &pms) {
  latest = pms;
  aqlogger::poll();
}
void lock_display() { aqlogger::lock_bus(); }
void unlock_display() { aqlogger::unlock_bus(); }
bool start_links(bool display_detected) {
  return aqlogger::start_links(display_detected);
}
bool enqueue_request(const ble::ControlRequest &request) {
  return aqlogger::enqueue_request(request);
}
const char *station_text_id() { return aqlogger::station_text_id(); }
const char *device_text_id() { return aqlogger::device_text_id(); }
const char *firmware_text_id() { return aqlogger::firmware_text_id(); }
ClockView clock_view() {
  const auto view = aqlogger::clock_view();
  return {view.utc_ns, view.epoch, view.source, view.rtc_state,
          view.source_name};
}
} // namespace telemetry
