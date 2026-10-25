// SEM(name, ucum, role, cf_standard_name, aggregate, plausible_min,
//     plausible_max, settle_s, valid_when, property, sensor_vendor,
//     sensor_model).
// One row per FIELD row in telemetry_fields.inc, same order, same name. The
// static_assert in telemetry_contract.h fails the build when they drift.
// This file is NOT part of the dict digest, which hashes telemetry_fields.inc
// only, so editing a value here never changes the published dictionary.
// Values follow the stac-opensensor v0.1.0 column profile and feed the
// generated board template (#85). Labels, display step and scales (WHO, AQI)
// belong to the app profile and the scale registry, never to this file.
// Per field precision stays out until the next dict bump (#91).
//
// Column meanings.
//   ucum           UCUM code. Annotations like {code} mark unitless labels.
//   role           ACDD coverage_content_type.
//   aggregate      CF cell method a daily or hourly roll up should use.
//   plausible      Bounds outside which a value is suspect, never masked.
//                  Both empty when no sourced bound exists.
//   settle_s       Seconds after boot before the column is trusted.
//   valid_when     "" no gate, "finite" the value itself must be finite,
//                  or "<column> == <n>". A gate that needs more than one
//                  column becomes "finite", since the firmware writes null
//                  whenever its own validity rule fails.
//   property       "" means
//                  urn:walkthru-earth:waveshare-sim7670g:property:<name>.
//
// Sources.
//   Gates and settle times are copied from opensensor-space-mobile
//   core/history Metrics.kt and core/protocol LiveProfile.kt, so the mobile
//   parity tests (#184) keep passing. PMS_SETTLE_S = 180 was measured on the
//   bench over 14 boots on 2026-09-17. The PMS5003T datasheet only promises
//   30 s, and the firmware already nulls those 30 s itself (kPmsWarmupUs).
//   Mobile gives ambient temperature and humidity no gate and no settle, so
//   they use "finite" and 0 here.
//   pms_status 4 is valid and pm25_batch_candidate_status 3 is
//   candidate-only, per main.cpp kMetadata.
//   PMS5003T datasheet PTQ3007-2015 V1.1, 2019-11-06
//   (docs/boards/waveshare-esp32-s3-sim7670g/pms5003t.md) gives PM maximum
//   range 1000 ug/m3 and operating temperature -10 to 60 degC. Its humidity
//   range is 0 to 99 %, and the firmware writes 0 to 100, so the bound is
//   0 to 100. Counts are one unsigned 16 bit frame word, so 0 to 65535.
//   MAX17048 (Analog Devices) bounds come from firmware/common/sensors
//   aq_max17048.h. VCELL 2500 to 4500 mV is the read check, raw SOC is
//   checked against 120 %, and battery_percent is written only within 0 to
//   100. battery_minutes_to_empty is capped at 7 days (10080 min) by
//   kMaxMinutesToEmpty in firmware/common/power aq_battery_health.cpp.
//   The batch candidate is a modelResult (0.003964 times
//   particles_gt03_per_01l). The opensensor:derived object it needs is
//   written by the template generator (#85), not here.
//   CF names come from the CF standard name table v86.
//
// Estimated, not sourced.
//   Every aggregate is a choice for this table, not a vendor value.
SEM(schema_version, "1", referenceInformation, "", mode, , , 0, "", "", "", "")
SEM(device_id, "1", referenceInformation, "", mode, , , 0, "", "", "", "")
SEM(boot_id_hi, "1", referenceInformation, "", mode, , , 0, "", "", "", "")
SEM(boot_id_lo, "1", referenceInformation, "", mode, , , 0, "", "", "", "")
SEM(sequence, "{count}", referenceInformation, "", point, , , 0, "", "", "", "")
SEM(monotonic_us, "us", referenceInformation, "", point, , , 0, "", "", "", "")
SEM(scheduled_us, "us", referenceInformation, "", point, , , 0, "", "", "", "")
SEM(sample_jitter_us, "us", auxiliaryInformation, "", mean, , , 0, "", "", "", "")
SEM(event_time_utc_us, "us", coordinate, "time", point, , , 0, "finite", "", "", "")
SEM(clock_status, "{code}", qualityInformation, "", mode, , , 0, "", "", "", "")
SEM(pms_status, "{code}", qualityInformation, "", mode, , , 0, "", "", "Plantower", "PMS5003T")
SEM(pms_age_ms, "ms", qualityInformation, "", point, , , 0, "finite", "", "Plantower", "PMS5003T")
SEM(pm1_cf1_ug_m3, "ug/m3", physicalMeasurement, "", mean, 0, 1000, 180, "pms_status == 4", "", "Plantower", "PMS5003T")
SEM(pm25_cf1_ug_m3, "ug/m3", physicalMeasurement, "", mean, 0, 1000, 180, "pms_status == 4", "", "Plantower", "PMS5003T")
SEM(pm10_cf1_ug_m3, "ug/m3", physicalMeasurement, "", mean, 0, 1000, 180, "pms_status == 4", "", "Plantower", "PMS5003T")
SEM(pm1_atmospheric_ug_m3, "ug/m3", physicalMeasurement, "mass_concentration_of_pm1_ambient_aerosol_particles_in_air", mean, 0, 1000, 180, "pms_status == 4", "", "Plantower", "PMS5003T")
SEM(pm25_atmospheric_ug_m3, "ug/m3", physicalMeasurement, "mass_concentration_of_pm2p5_ambient_aerosol_particles_in_air", mean, 0, 1000, 180, "pms_status == 4", "http://vocab.nerc.ac.uk/collection/P07/current/YUDG9DXK/", "Plantower", "PMS5003T")
SEM(pm10_atmospheric_ug_m3, "ug/m3", physicalMeasurement, "mass_concentration_of_pm10_ambient_aerosol_particles_in_air", mean, 0, 1000, 180, "pms_status == 4", "", "Plantower", "PMS5003T")
SEM(particles_gt03_per_01l, "{particles}/dL", physicalMeasurement, "", mean, 0, 65535, 180, "pms_status == 4", "", "Plantower", "PMS5003T")
SEM(particles_gt05_per_01l, "{particles}/dL", physicalMeasurement, "", mean, 0, 65535, 180, "pms_status == 4", "", "Plantower", "PMS5003T")
SEM(particles_gt10_per_01l, "{particles}/dL", physicalMeasurement, "", mean, 0, 65535, 180, "pms_status == 4", "", "Plantower", "PMS5003T")
SEM(particles_gt25_per_01l, "{particles}/dL", physicalMeasurement, "", mean, 0, 65535, 180, "pms_status == 4", "", "Plantower", "PMS5003T")
SEM(sensor_serial_code, "1", referenceInformation, "", mode, , , 0, "finite", "", "Plantower", "PMS5003T")
SEM(pm25_batch_candidate_ug_m3, "ug/m3", modelResult, "", mean, 0, 1000, 180, "pm25_batch_candidate_status == 3", "", "", "")
SEM(pm25_batch_candidate_status, "{code}", qualityInformation, "", mode, , , 0, "", "", "", "")
SEM(ambient_temperature_c, "Cel", physicalMeasurement, "air_temperature", mean, -10, 60, 0, "finite", "", "Plantower", "PMS5003T")
SEM(relative_humidity_percent, "%", physicalMeasurement, "relative_humidity", mean, 0, 100, 0, "finite", "", "Plantower", "PMS5003T")
SEM(pms_firmware, "{code}", referenceInformation, "", mode, , , 0, "finite", "", "Plantower", "PMS5003T")
SEM(pms_error, "{code}", qualityInformation, "", mode, , , 0, "finite", "", "Plantower", "PMS5003T")
SEM(pms_frames, "{count}", qualityInformation, "", point, , , 0, "", "", "Plantower", "PMS5003T")
SEM(pms_checksum_errors, "{count}", qualityInformation, "", point, , , 0, "", "", "Plantower", "PMS5003T")
SEM(pms_length_errors, "{count}", qualityInformation, "", point, , , 0, "", "", "Plantower", "PMS5003T")
SEM(gauge_status, "{code}", qualityInformation, "", mode, , , 0, "", "", "Analog Devices", "MAX17048")
SEM(battery_mv, "mV", auxiliaryInformation, "", mean, 2500, 4500, 0, "finite", "", "Analog Devices", "MAX17048")
SEM(battery_percent, "%", auxiliaryInformation, "", point, 0, 100, 0, "", "", "Analog Devices", "MAX17048")
SEM(heap_free_bytes, "By", auxiliaryInformation, "", minimum, , , 0, "", "", "Espressif", "ESP32-S3")
SEM(heap_min_free_bytes, "By", auxiliaryInformation, "", minimum, , , 0, "", "", "Espressif", "ESP32-S3")
SEM(psram_free_bytes, "By", auxiliaryInformation, "", minimum, , , 0, "", "", "Espressif", "ESP32-S3")
SEM(sd_total_bytes, "By", auxiliaryInformation, "", point, , , 0, "finite", "", "", "")
SEM(sd_used_bytes, "By", auxiliaryInformation, "", point, , , 0, "finite", "", "", "")
SEM(rows_dropped, "{count}", auxiliaryInformation, "", point, , , 0, "", "", "", "")
SEM(sample_deadlines_missed, "{count}", auxiliaryInformation, "", point, , , 0, "", "", "", "")
SEM(storage_errors, "{count}", auxiliaryInformation, "", point, , , 0, "", "", "", "")
SEM(files_finalized, "{count}", auxiliaryInformation, "", point, , , 0, "", "", "", "")
SEM(last_write_us, "us", auxiliaryInformation, "", point, , , 0, "", "", "", "")
SEM(last_sync_us, "us", auxiliaryInformation, "", point, , , 0, "", "", "", "")
SEM(queue_high_water, "{count}", auxiliaryInformation, "", maximum, , , 0, "", "", "", "")
SEM(clock_epoch, "{count}", referenceInformation, "", point, , , 0, "", "", "", "")
SEM(collection_completed_mono_us, "us", referenceInformation, "", point, , , 0, "", "", "", "")
SEM(pms_received_mono_us, "us", referenceInformation, "", point, , , 0, "finite", "", "Plantower", "PMS5003T")
SEM(clock_anchor_mono_us, "us", referenceInformation, "", point, , , 0, "finite", "", "", "")
SEM(clock_anchor_utc_us, "us", referenceInformation, "", point, , , 0, "finite", "", "", "")
SEM(h3_cell_id, "{h3_index}", coordinate, "", mode, , , 0, "finite", "", "", "")
SEM(h3_resolution, "1", referenceInformation, "", mode, 0, 15, 0, "finite", "", "", "")
SEM(h3_center_lat_e7, "10*-7.deg", coordinate, "", mode, -900000000, 900000000, 0, "finite", "", "", "")
SEM(h3_center_lon_e7, "10*-7.deg", coordinate, "", mode, -1800000000, 1800000000, 0, "finite", "", "", "")
SEM(battery_soc_raw_percent, "%", auxiliaryInformation, "", point, 0, 120, 0, "finite", "", "Analog Devices", "MAX17048")
SEM(battery_charge_rate_percent_per_h, "%/h", auxiliaryInformation, "", mean, , , 0, "finite", "", "Analog Devices", "MAX17048")
SEM(gauge_status_flags, "{bitmask}", qualityInformation, "", mode, , , 0, "finite", "", "Analog Devices", "MAX17048")
SEM(gauge_hibernating, "{boolean}", auxiliaryInformation, "", mode, , , 0, "finite", "", "Analog Devices", "MAX17048")
SEM(battery_health_state, "{code}", auxiliaryInformation, "", mode, , , 0, "finite", "", "", "")
SEM(battery_minutes_to_empty, "min", auxiliaryInformation, "", point, 0, 10080, 0, "finite", "", "", "")
