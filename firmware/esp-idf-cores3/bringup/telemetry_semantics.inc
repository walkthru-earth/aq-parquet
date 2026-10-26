// SEM(name, ucum, role, cf_standard_name, aggregate, plausible_min,
//     plausible_max, settle_s, valid_when, property, sensor_vendor,
//     sensor_model).
// One row per FIELD row in telemetry_fields.inc, same order, same name. The
// static_assert in telemetry_contract.h fails the build when they drift.
// This file is NOT part of the dict digest, which hashes telemetry_fields.inc
// only, so editing a row never moves the digest. The property column does
// reach the FIELD table, the `parquet schema` console output and the host
// dictionary JSON. The old board URN in the telemetry_fields.inc header
// comment is stale until the next dict bump (#91), since editing it moves
// the digest.
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
//   settle_s       Seconds after the sensor wakes, not after boot, before
//                  the column is trusted. A duty cycled sensor (#39) settles
//                  again on every wake.
//   valid_when     "" no gate, "finite" the value itself must be finite,
//                  or "<column> == <n>". A gate that needs more than one
//                  column becomes "finite", since the firmware writes null
//                  whenever its own validity rule fails. A row that passes
//                  its gate within settle_s of the sensor waking counts but
//                  is flagged as settling.
//   property       Board neutral id, and the single source of the FIELD
//                  property_uri in telemetry_contract.h. A NERC P07 term
//                  first, then the NERC CF standard_name URI for an exact CF
//                  match, else urn:walkthru-earth:property:<quantity>. Never
//                  a board or sensor name, those go in opensensor:procedure.
//                  The same quantity has the same id on every board.
//
// Sources.
//   Units, gates and settle times are copied from opensensor-space-mobile
//   core/history Metrics.kt (7f70fb72), so the mobile parity tests (#184)
//   keep passing. tools/test_column_semantics.cpp checks each copied value
//   against its Metrics.kt line. LiveProfile.kt carries only LIVE keys and
//   digests, no semantic values. A Kotlin null gate is "finite" here for
//   measurements, since MetricSpec.valueOf drops a non-finite value.
//   PMS_SETTLE_S = 180 was measured on the
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
//   imu_temperature_c uses -40 to 85, the BMI270 operating range from the
//   Bosch Sensortec BMI270 datasheet. The stac-opensensor
//   cores3-telemetry-v5.json example says -20 to 85, and the sensor fact wins.
//   SHT20 range -40 to 125 degC and 0 to 100 %RH from the Sensirion SHT20
//   datasheet. The columns stay null until the sensor is resolved.
//   CF names come from the CF standard name table v86. Property ids follow
//   stac-opensensor PROFILE.md (a017edf), and the five PM and count ids its
//   cores3-telemetry-v5.json template assigns are copied verbatim. P07
//   CFSN0115 is time, CFSN0023 air_temperature, CFSN0413 relative_humidity.
//
// New, not in Metrics.kt.
//   Every plausible range, every aggregate, every property id not in the
//   template, and the gate and settle of columns Metrics.kt does not list.
//   Particle columns Metrics.kt does not list take PMS_VALID and
//   PMS_SETTLE_S, which Metrics.kt shares with every particle metric.
//
// Estimated, not sourced.
//   vbus_mv 0 to 5500 and battery_mv 2500 to 4500 are bench estimates for a
//   5 V USB rail and a single Li-ion cell behind the AXP2101, not datasheet
//   limits. Their rows carry an "estimate" comment.
//   Every aggregate is a choice for this table, not a vendor value. Running
//   counters (frames, errors, dropped rows) use point because the profile
//   has no counter delta aggregate.
SEM(schema_version, "1", referenceInformation, "", mode, , , 0, "", "urn:walkthru-earth:property:schema_version", "", "")
SEM(device_id, "1", referenceInformation, "", mode, , , 0, "", "urn:walkthru-earth:property:device_identifier", "", "")
SEM(boot_id_hi, "1", referenceInformation, "", mode, , , 0, "", "urn:walkthru-earth:property:boot_identifier_high", "", "")
SEM(boot_id_lo, "1", referenceInformation, "", mode, , , 0, "", "urn:walkthru-earth:property:boot_identifier_low", "", "")
SEM(sequence, "{count}", referenceInformation, "", point, , , 0, "", "urn:walkthru-earth:property:sample_sequence_number", "", "")
SEM(monotonic_us, "us", referenceInformation, "", point, , , 0, "", "urn:walkthru-earth:property:monotonic_time_since_boot", "", "")
SEM(scheduled_us, "us", referenceInformation, "", point, , , 0, "", "urn:walkthru-earth:property:scheduled_sample_time_since_boot", "", "")
SEM(sample_jitter_us, "us", auxiliaryInformation, "", mean, , , 0, "", "urn:walkthru-earth:property:sample_jitter", "", "")
SEM(event_time_utc_us, "us", coordinate, "time", point, , , 0, "", "http://vocab.nerc.ac.uk/collection/P07/current/CFSN0115/", "", "")
SEM(clock_status, "{code}", qualityInformation, "", mode, , , 0, "", "urn:walkthru-earth:property:clock_status", "", "")
SEM(pms_status, "{code}", qualityInformation, "", mode, , , 0, "", "urn:walkthru-earth:property:particle_sensor_status", "Plantower", "PMSA003")
SEM(pms_age_ms, "ms", qualityInformation, "", point, , , 0, "finite", "urn:walkthru-earth:property:particle_sensor_frame_age", "Plantower", "PMSA003")
SEM(pm1_cf1_ug_m3, "ug/m3", physicalMeasurement, "", mean, 0, 1000, 180, "pms_status == 4", "urn:walkthru-earth:property:pm1_cf1_mass_concentration", "Plantower", "PMSA003")
SEM(pm25_cf1_ug_m3, "ug/m3", physicalMeasurement, "", mean, 0, 1000, 180, "pms_status == 4", "urn:walkthru-earth:property:pm25_cf1_mass_concentration", "Plantower", "PMSA003")
SEM(pm10_cf1_ug_m3, "ug/m3", physicalMeasurement, "", mean, 0, 1000, 180, "pms_status == 4", "urn:walkthru-earth:property:pm10_cf1_mass_concentration", "Plantower", "PMSA003")
SEM(pm1_atmospheric_ug_m3, "ug/m3", physicalMeasurement, "mass_concentration_of_pm1_ambient_aerosol_particles_in_air", mean, 0, 1000, 180, "pms_status == 4", "http://vocab.nerc.ac.uk/standard_name/mass_concentration_of_pm1_ambient_aerosol_particles_in_air/", "Plantower", "PMSA003")
SEM(pm25_atmospheric_ug_m3, "ug/m3", physicalMeasurement, "mass_concentration_of_pm2p5_ambient_aerosol_particles_in_air", mean, 0, 1000, 180, "pms_status == 4", "http://vocab.nerc.ac.uk/collection/P07/current/YUDG9DXK/", "Plantower", "PMSA003")
SEM(pm10_atmospheric_ug_m3, "ug/m3", physicalMeasurement, "mass_concentration_of_pm10_ambient_aerosol_particles_in_air", mean, 0, 1000, 180, "pms_status == 4", "http://vocab.nerc.ac.uk/standard_name/mass_concentration_of_pm10_ambient_aerosol_particles_in_air/", "Plantower", "PMSA003")
SEM(particles_gt03_per_01l, "{particles}/dL", physicalMeasurement, "", mean, 0, 65535, 180, "pms_status == 4", "urn:walkthru-earth:property:particle_count_gt_0p3um", "Plantower", "PMSA003")
SEM(particles_gt05_per_01l, "{particles}/dL", physicalMeasurement, "", mean, 0, 65535, 180, "pms_status == 4", "urn:walkthru-earth:property:particle_count_gt_0p5um", "Plantower", "PMSA003")
SEM(particles_gt10_per_01l, "{particles}/dL", physicalMeasurement, "", mean, 0, 65535, 180, "pms_status == 4", "urn:walkthru-earth:property:particle_count_gt_1um", "Plantower", "PMSA003")
SEM(particles_gt25_per_01l, "{particles}/dL", physicalMeasurement, "", mean, 0, 65535, 180, "pms_status == 4", "urn:walkthru-earth:property:particle_count_gt_2p5um", "Plantower", "PMSA003")
SEM(particles_gt50_per_01l, "{particles}/dL", physicalMeasurement, "", mean, 0, 65535, 180, "pms_status == 4", "urn:walkthru-earth:property:particle_count_gt_5um", "Plantower", "PMSA003")
SEM(particles_gt100_per_01l, "{particles}/dL", physicalMeasurement, "", mean, 0, 65535, 180, "pms_status == 4", "urn:walkthru-earth:property:particle_count_gt_10um", "Plantower", "PMSA003")
SEM(pms_firmware, "{code}", referenceInformation, "", mode, , , 0, "finite", "urn:walkthru-earth:property:particle_sensor_firmware_version", "Plantower", "PMSA003")
SEM(pms_error, "{code}", qualityInformation, "", mode, , , 0, "finite", "urn:walkthru-earth:property:particle_sensor_error_code", "Plantower", "PMSA003")
SEM(pms_frames, "{count}", qualityInformation, "", point, , , 0, "", "urn:walkthru-earth:property:particle_sensor_frame_count", "Plantower", "PMSA003")
SEM(pms_checksum_errors, "{count}", qualityInformation, "", point, , , 0, "", "urn:walkthru-earth:property:particle_sensor_checksum_error_count", "Plantower", "PMSA003")
SEM(pms_length_errors, "{count}", qualityInformation, "", point, , , 0, "", "urn:walkthru-earth:property:particle_sensor_length_error_count", "Plantower", "PMSA003")
SEM(imu_fresh_mask, "{bitmask}", qualityInformation, "", mode, , , 0, "", "urn:walkthru-earth:property:motion_sensor_fresh_mask", "Bosch Sensortec", "BMI270")
SEM(accel_x_g, "[g]", auxiliaryInformation, "", mean, , , 0, "finite", "urn:walkthru-earth:property:acceleration_x", "Bosch Sensortec", "BMI270")
SEM(accel_y_g, "[g]", auxiliaryInformation, "", mean, , , 0, "finite", "urn:walkthru-earth:property:acceleration_y", "Bosch Sensortec", "BMI270")
SEM(accel_z_g, "[g]", auxiliaryInformation, "", mean, , , 0, "finite", "urn:walkthru-earth:property:acceleration_z", "Bosch Sensortec", "BMI270")
SEM(gyro_x_dps, "deg/s", auxiliaryInformation, "", mean, , , 0, "finite", "urn:walkthru-earth:property:angular_rate_x", "Bosch Sensortec", "BMI270")
SEM(gyro_y_dps, "deg/s", auxiliaryInformation, "", mean, , , 0, "finite", "urn:walkthru-earth:property:angular_rate_y", "Bosch Sensortec", "BMI270")
SEM(gyro_z_dps, "deg/s", auxiliaryInformation, "", mean, , , 0, "finite", "urn:walkthru-earth:property:angular_rate_z", "Bosch Sensortec", "BMI270")
SEM(mag_x_raw, "{raw_count}", auxiliaryInformation, "", mean, , , 0, "finite", "urn:walkthru-earth:property:magnetic_field_x_raw", "Bosch Sensortec", "BMM150")
SEM(mag_y_raw, "{raw_count}", auxiliaryInformation, "", mean, , , 0, "finite", "urn:walkthru-earth:property:magnetic_field_y_raw", "Bosch Sensortec", "BMM150")
SEM(mag_z_raw, "{raw_count}", auxiliaryInformation, "", mean, , , 0, "finite", "urn:walkthru-earth:property:magnetic_field_z_raw", "Bosch Sensortec", "BMM150")
SEM(imu_temperature_c, "Cel", auxiliaryInformation, "", mean, -40, 85, 0, "finite", "urn:walkthru-earth:property:board_temperature", "Bosch Sensortec", "BMI270")
SEM(light_ch0_raw, "{raw_count}", physicalMeasurement, "", mean, 0, 65535, 0, "light_status == 3", "urn:walkthru-earth:property:ambient_light_visible_and_infrared_raw", "Lite-On", "LTR-553ALS-WA")
SEM(light_ch1_raw, "{raw_count}", physicalMeasurement, "", mean, 0, 65535, 0, "light_status == 3", "urn:walkthru-earth:property:ambient_light_infrared_raw", "Lite-On", "LTR-553ALS-WA")
SEM(proximity_raw, "{raw_count}", physicalMeasurement, "", mean, 0, 2047, 0, "proximity_status == 3", "urn:walkthru-earth:property:proximity_raw", "Lite-On", "LTR-553ALS-WA")
SEM(light_status, "{code}", qualityInformation, "", mode, , , 0, "", "urn:walkthru-earth:property:ambient_light_status", "Lite-On", "LTR-553ALS-WA")
SEM(proximity_status, "{code}", qualityInformation, "", mode, , , 0, "", "urn:walkthru-earth:property:proximity_status", "Lite-On", "LTR-553ALS-WA")
// Estimate, see the header.
SEM(vbus_mv, "mV", auxiliaryInformation, "", mean, 0, 5500, 0, "", "urn:walkthru-earth:property:usb_bus_voltage", "X-Powers", "AXP2101")
// Estimate, see the header.
SEM(battery_mv, "mV", auxiliaryInformation, "", mean, 2500, 4500, 0, "", "urn:walkthru-earth:property:battery_voltage", "X-Powers", "AXP2101")
SEM(battery_percent, "%", auxiliaryInformation, "", point, 0, 100, 0, "", "urn:walkthru-earth:property:battery_state_of_charge", "X-Powers", "AXP2101")
SEM(charging_status, "{code}", auxiliaryInformation, "", mode, , , 0, "", "urn:walkthru-earth:property:battery_charging_status", "X-Powers", "AXP2101")
SEM(battery_current_ma, "mA", auxiliaryInformation, "", mean, , , 0, "finite", "urn:walkthru-earth:property:battery_current", "X-Powers", "AXP2101")
SEM(external_5v_enabled, "{boolean}", auxiliaryInformation, "", mode, , , 0, "", "urn:walkthru-earth:property:external_5v_output_enabled", "X-Powers", "AXP2101")
SEM(usb_output_enabled, "{boolean}", auxiliaryInformation, "", mode, , , 0, "", "urn:walkthru-earth:property:usb_output_enabled", "X-Powers", "AXP2101")
SEM(rtc_date_yyyymmdd, "{YYYYMMDD}", referenceInformation, "", point, , , 0, "rtc_read_ok == 1", "urn:walkthru-earth:property:real_time_clock_date", "Belling", "BM8563")
SEM(rtc_time_hhmmss, "{hhmmss}", referenceInformation, "", point, , , 0, "rtc_read_ok == 1", "urn:walkthru-earth:property:real_time_clock_time", "Belling", "BM8563")
SEM(rtc_read_ok, "{boolean}", qualityInformation, "", mode, , , 0, "", "urn:walkthru-earth:property:real_time_clock_read_status", "Belling", "BM8563")
SEM(touch_points, "{count}", auxiliaryInformation, "", maximum, , , 0, "", "urn:walkthru-earth:property:touch_point_count", "FocalTech", "FT6336U")
SEM(touch_x_px, "{pixel}", auxiliaryInformation, "", point, 0, 319, 0, "finite", "urn:walkthru-earth:property:touch_x_position", "FocalTech", "FT6336U")
SEM(touch_y_px, "{pixel}", auxiliaryInformation, "", point, 0, 239, 0, "finite", "urn:walkthru-earth:property:touch_y_position", "FocalTech", "FT6336U")
SEM(heap_free_bytes, "By", auxiliaryInformation, "", minimum, , , 0, "", "urn:walkthru-earth:property:free_heap_memory", "Espressif", "ESP32-S3")
SEM(heap_min_free_bytes, "By", auxiliaryInformation, "", minimum, , , 0, "", "urn:walkthru-earth:property:minimum_free_heap_memory", "Espressif", "ESP32-S3")
SEM(psram_free_bytes, "By", auxiliaryInformation, "", minimum, , , 0, "", "urn:walkthru-earth:property:free_psram_memory", "Espressif", "ESP32-S3")
SEM(sd_total_bytes, "By", auxiliaryInformation, "", point, , , 0, "finite", "urn:walkthru-earth:property:storage_capacity", "", "")
SEM(sd_used_bytes, "By", auxiliaryInformation, "", point, , , 0, "finite", "urn:walkthru-earth:property:storage_used", "", "")
SEM(rows_dropped, "{count}", auxiliaryInformation, "", point, , , 0, "", "urn:walkthru-earth:property:dropped_row_count", "", "")
SEM(sample_deadlines_missed, "{count}", auxiliaryInformation, "", point, , , 0, "", "urn:walkthru-earth:property:missed_sample_deadline_count", "", "")
SEM(storage_errors, "{count}", auxiliaryInformation, "", point, , , 0, "", "urn:walkthru-earth:property:storage_error_count", "", "")
SEM(files_finalized, "{count}", auxiliaryInformation, "", point, , , 0, "", "urn:walkthru-earth:property:finalized_file_count", "", "")
SEM(last_write_us, "us", auxiliaryInformation, "", point, , , 0, "", "urn:walkthru-earth:property:last_storage_write_time", "", "")
SEM(last_sync_us, "us", auxiliaryInformation, "", point, , , 0, "", "urn:walkthru-earth:property:last_storage_sync_time", "", "")
SEM(queue_high_water, "{count}", auxiliaryInformation, "", maximum, , , 0, "", "urn:walkthru-earth:property:queue_high_water_mark", "", "")
SEM(ambient_temperature_c, "Cel", physicalMeasurement, "air_temperature", mean, -40, 125, 0, "finite", "http://vocab.nerc.ac.uk/collection/P07/current/CFSN0023/", "Sensirion", "SHT20")
SEM(relative_humidity_percent, "%", physicalMeasurement, "relative_humidity", mean, 0, 100, 0, "finite", "http://vocab.nerc.ac.uk/collection/P07/current/CFSN0413/", "Sensirion", "SHT20")
SEM(clock_epoch, "{count}", referenceInformation, "", point, , , 0, "", "urn:walkthru-earth:property:clock_epoch", "", "")
SEM(collection_completed_mono_us, "us", referenceInformation, "", point, , , 0, "", "urn:walkthru-earth:property:collection_completed_time_since_boot", "", "")
SEM(pms_received_mono_us, "us", referenceInformation, "", point, , , 0, "finite", "urn:walkthru-earth:property:particle_sensor_frame_received_time_since_boot", "Plantower", "PMSA003")
SEM(clock_anchor_mono_us, "us", referenceInformation, "", point, , , 0, "finite", "urn:walkthru-earth:property:clock_anchor_time_since_boot", "", "")
SEM(clock_anchor_utc_us, "us", referenceInformation, "", point, , , 0, "finite", "urn:walkthru-earth:property:clock_anchor_utc_time", "", "")
SEM(h3_cell_id, "{h3_index}", coordinate, "", mode, , , 0, "finite", "urn:walkthru-earth:property:h3_cell_index", "", "")
SEM(h3_resolution, "1", referenceInformation, "", mode, 0, 15, 0, "finite", "urn:walkthru-earth:property:h3_resolution", "", "")
SEM(location_source, "{code}", referenceInformation, "", mode, , , 0, "finite", "urn:walkthru-earth:property:location_source", "", "")
SEM(geometry, "deg", coordinate, "", mode, , , 0, "", "urn:walkthru-earth:property:h3_cell_center_point", "", "")
