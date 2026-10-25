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
//   property       "" means urn:walkthru-earth:cores3:property:<name>.
//
// Sources.
//   Gates and settle times are copied from opensensor-space-mobile
//   core/history Metrics.kt and core/protocol LiveProfile.kt, so the mobile
//   parity tests (#184) keep passing. PMS_SETTLE_S = 180 was measured on the
//   bench over 14 boots on 2026-09-17. The PMSA003 datasheet only promises
//   30 s, and the firmware already nulls those 30 s itself.
//   Status codes come from telemetry_logger.cpp collect(). light_status and
//   proximity_status use 3 for valid, rtc_read_ok uses 1, pms_status uses 4.
//   PMSA003 datasheet V2.5 (docs/boards/m5stack-cores3/addon-air-quality.md)
//   gives PM 0 to 1000 ug/m3 as the maximum range. Counts are one unsigned
//   16 bit frame word, so 0 to 65535.
//   Chips per docs/boards/m5stack-cores3/cores3-hardware.md. BMI270 and
//   BMM150 (Bosch Sensortec), LTR-553ALS-WA (Lite-On), AXP2101 (X-Powers),
//   BM8563 (Belling), FT6336U (FocalTech). LTR-553 ALS is 16 bit and PS is
//   11 bit. The LCD is 320 by 240, so touch x is 0 to 319 and y 0 to 239.
//   imu_temperature_c uses -20 to 85, copied from the stac-opensensor
//   templates/cores3-telemetry-v5.json example. The BMI270 operating range is
//   -40 to 85, so the lower bound is the profile's choice.
//   SHT20 range -40 to 125 degC and 0 to 100 %RH from the Sensirion SHT20
//   datasheet. The columns stay null until the sensor is resolved.
//   CF names come from the CF standard name table v86.
//
// Estimated, not sourced.
//   vbus_mv 0 to 5500 and battery_mv 2500 to 4500 are bench estimates for a
//   5 V USB rail and a single Li-ion cell behind the AXP2101.
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
SEM(pms_status, "{code}", qualityInformation, "", mode, , , 0, "", "", "Plantower", "PMSA003")
SEM(pms_age_ms, "ms", qualityInformation, "", point, , , 0, "finite", "", "Plantower", "PMSA003")
SEM(pm1_cf1_ug_m3, "ug/m3", physicalMeasurement, "", mean, 0, 1000, 180, "pms_status == 4", "", "Plantower", "PMSA003")
SEM(pm25_cf1_ug_m3, "ug/m3", physicalMeasurement, "", mean, 0, 1000, 180, "pms_status == 4", "", "Plantower", "PMSA003")
SEM(pm10_cf1_ug_m3, "ug/m3", physicalMeasurement, "", mean, 0, 1000, 180, "pms_status == 4", "", "Plantower", "PMSA003")
SEM(pm1_atmospheric_ug_m3, "ug/m3", physicalMeasurement, "mass_concentration_of_pm1_ambient_aerosol_particles_in_air", mean, 0, 1000, 180, "pms_status == 4", "", "Plantower", "PMSA003")
SEM(pm25_atmospheric_ug_m3, "ug/m3", physicalMeasurement, "mass_concentration_of_pm2p5_ambient_aerosol_particles_in_air", mean, 0, 1000, 180, "pms_status == 4", "http://vocab.nerc.ac.uk/collection/P07/current/YUDG9DXK/", "Plantower", "PMSA003")
SEM(pm10_atmospheric_ug_m3, "ug/m3", physicalMeasurement, "mass_concentration_of_pm10_ambient_aerosol_particles_in_air", mean, 0, 1000, 180, "pms_status == 4", "", "Plantower", "PMSA003")
SEM(particles_gt03_per_01l, "{particles}/dL", physicalMeasurement, "", mean, 0, 65535, 180, "pms_status == 4", "", "Plantower", "PMSA003")
SEM(particles_gt05_per_01l, "{particles}/dL", physicalMeasurement, "", mean, 0, 65535, 180, "pms_status == 4", "", "Plantower", "PMSA003")
SEM(particles_gt10_per_01l, "{particles}/dL", physicalMeasurement, "", mean, 0, 65535, 180, "pms_status == 4", "", "Plantower", "PMSA003")
SEM(particles_gt25_per_01l, "{particles}/dL", physicalMeasurement, "", mean, 0, 65535, 180, "pms_status == 4", "", "Plantower", "PMSA003")
SEM(particles_gt50_per_01l, "{particles}/dL", physicalMeasurement, "", mean, 0, 65535, 180, "pms_status == 4", "", "Plantower", "PMSA003")
SEM(particles_gt100_per_01l, "{particles}/dL", physicalMeasurement, "", mean, 0, 65535, 180, "pms_status == 4", "", "Plantower", "PMSA003")
SEM(pms_firmware, "{code}", referenceInformation, "", mode, , , 0, "finite", "", "Plantower", "PMSA003")
SEM(pms_error, "{code}", qualityInformation, "", mode, , , 0, "finite", "", "Plantower", "PMSA003")
SEM(pms_frames, "{count}", qualityInformation, "", point, , , 0, "", "", "Plantower", "PMSA003")
SEM(pms_checksum_errors, "{count}", qualityInformation, "", point, , , 0, "", "", "Plantower", "PMSA003")
SEM(pms_length_errors, "{count}", qualityInformation, "", point, , , 0, "", "", "Plantower", "PMSA003")
SEM(imu_fresh_mask, "{bitmask}", qualityInformation, "", mode, , , 0, "", "", "Bosch Sensortec", "BMI270")
SEM(accel_x_g, "[g]", auxiliaryInformation, "", mean, , , 0, "finite", "", "Bosch Sensortec", "BMI270")
SEM(accel_y_g, "[g]", auxiliaryInformation, "", mean, , , 0, "finite", "", "Bosch Sensortec", "BMI270")
SEM(accel_z_g, "[g]", auxiliaryInformation, "", mean, , , 0, "finite", "", "Bosch Sensortec", "BMI270")
SEM(gyro_x_dps, "deg/s", auxiliaryInformation, "", mean, , , 0, "finite", "", "Bosch Sensortec", "BMI270")
SEM(gyro_y_dps, "deg/s", auxiliaryInformation, "", mean, , , 0, "finite", "", "Bosch Sensortec", "BMI270")
SEM(gyro_z_dps, "deg/s", auxiliaryInformation, "", mean, , , 0, "finite", "", "Bosch Sensortec", "BMI270")
SEM(mag_x_raw, "{raw_count}", auxiliaryInformation, "", mean, , , 0, "finite", "", "Bosch Sensortec", "BMM150")
SEM(mag_y_raw, "{raw_count}", auxiliaryInformation, "", mean, , , 0, "finite", "", "Bosch Sensortec", "BMM150")
SEM(mag_z_raw, "{raw_count}", auxiliaryInformation, "", mean, , , 0, "finite", "", "Bosch Sensortec", "BMM150")
SEM(imu_temperature_c, "Cel", auxiliaryInformation, "", mean, -20, 85, 0, "finite", "", "Bosch Sensortec", "BMI270")
SEM(light_ch0_raw, "{raw_count}", physicalMeasurement, "", mean, 0, 65535, 0, "light_status == 3", "", "Lite-On", "LTR-553ALS-WA")
SEM(light_ch1_raw, "{raw_count}", physicalMeasurement, "", mean, 0, 65535, 0, "light_status == 3", "", "Lite-On", "LTR-553ALS-WA")
SEM(proximity_raw, "{raw_count}", physicalMeasurement, "", mean, 0, 2047, 0, "proximity_status == 3", "", "Lite-On", "LTR-553ALS-WA")
SEM(light_status, "{code}", qualityInformation, "", mode, , , 0, "", "", "Lite-On", "LTR-553ALS-WA")
SEM(proximity_status, "{code}", qualityInformation, "", mode, , , 0, "", "", "Lite-On", "LTR-553ALS-WA")
SEM(vbus_mv, "mV", auxiliaryInformation, "", mean, 0, 5500, 0, "", "", "X-Powers", "AXP2101")
SEM(battery_mv, "mV", auxiliaryInformation, "", mean, 2500, 4500, 0, "", "", "X-Powers", "AXP2101")
SEM(battery_percent, "%", auxiliaryInformation, "", point, 0, 100, 0, "", "", "X-Powers", "AXP2101")
SEM(charging_status, "{code}", auxiliaryInformation, "", mode, , , 0, "", "", "X-Powers", "AXP2101")
SEM(battery_current_ma, "mA", auxiliaryInformation, "", mean, , , 0, "finite", "", "X-Powers", "AXP2101")
SEM(external_5v_enabled, "{boolean}", auxiliaryInformation, "", mode, , , 0, "", "", "X-Powers", "AXP2101")
SEM(usb_output_enabled, "{boolean}", auxiliaryInformation, "", mode, , , 0, "", "", "X-Powers", "AXP2101")
SEM(rtc_date_yyyymmdd, "{YYYYMMDD}", referenceInformation, "", point, , , 0, "rtc_read_ok == 1", "", "Belling", "BM8563")
SEM(rtc_time_hhmmss, "{hhmmss}", referenceInformation, "", point, , , 0, "rtc_read_ok == 1", "", "Belling", "BM8563")
SEM(rtc_read_ok, "{boolean}", qualityInformation, "", mode, , , 0, "", "", "Belling", "BM8563")
SEM(touch_points, "{count}", auxiliaryInformation, "", maximum, , , 0, "", "", "FocalTech", "FT6336U")
SEM(touch_x_px, "{pixel}", auxiliaryInformation, "", point, 0, 319, 0, "finite", "", "FocalTech", "FT6336U")
SEM(touch_y_px, "{pixel}", auxiliaryInformation, "", point, 0, 239, 0, "finite", "", "FocalTech", "FT6336U")
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
SEM(ambient_temperature_c, "Cel", physicalMeasurement, "air_temperature", mean, -40, 125, 0, "finite", "", "Sensirion", "SHT20")
SEM(relative_humidity_percent, "%", physicalMeasurement, "relative_humidity", mean, 0, 100, 0, "finite", "", "Sensirion", "SHT20")
SEM(clock_epoch, "{count}", referenceInformation, "", point, , , 0, "", "", "", "")
SEM(collection_completed_mono_us, "us", referenceInformation, "", point, , , 0, "", "", "", "")
SEM(pms_received_mono_us, "us", referenceInformation, "", point, , , 0, "finite", "", "Plantower", "PMSA003")
SEM(clock_anchor_mono_us, "us", referenceInformation, "", point, , , 0, "finite", "", "", "")
SEM(clock_anchor_utc_us, "us", referenceInformation, "", point, , , 0, "finite", "", "", "")
SEM(h3_cell_id, "{h3_index}", coordinate, "", mode, , , 0, "finite", "", "", "")
SEM(h3_resolution, "1", referenceInformation, "", mode, 0, 15, 0, "finite", "", "", "")
SEM(h3_center_lat_e7, "10*-7.deg", coordinate, "", mode, -900000000, 900000000, 0, "finite", "", "", "")
SEM(h3_center_lon_e7, "10*-7.deg", coordinate, "", mode, -1800000000, 1800000000, 0, "finite", "", "", "")
