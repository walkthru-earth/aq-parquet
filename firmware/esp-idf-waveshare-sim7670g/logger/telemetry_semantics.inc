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
//   os_max17048.h. VCELL 2500 to 4500 mV is the read check, raw SOC is
//   checked against 120 %, and battery_percent is written only within 0 to
//   100. battery_minutes_to_empty is capped at 7 days (10080 min) by
//   kMaxMinutesToEmpty in firmware/common/power os_battery_health.cpp.
//   The batch candidate is a modelResult (0.003964 times
//   particles_gt03_per_01l). The opensensor:derived object it needs is
//   written by the template generator (#85), not here.
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
SEM(pms_status, "{code}", qualityInformation, "", mode, , , 0, "", "urn:walkthru-earth:property:particle_sensor_status", "Plantower", "PMS5003T")
SEM(pms_age_ms, "ms", qualityInformation, "", point, , , 0, "finite", "urn:walkthru-earth:property:particle_sensor_frame_age", "Plantower", "PMS5003T")
SEM(pm1_cf1_ug_m3, "ug/m3", physicalMeasurement, "", mean, 0, 1000, 180, "pms_status == 4", "urn:walkthru-earth:property:pm1_cf1_mass_concentration", "Plantower", "PMS5003T")
SEM(pm25_cf1_ug_m3, "ug/m3", physicalMeasurement, "", mean, 0, 1000, 180, "pms_status == 4", "urn:walkthru-earth:property:pm25_cf1_mass_concentration", "Plantower", "PMS5003T")
SEM(pm10_cf1_ug_m3, "ug/m3", physicalMeasurement, "", mean, 0, 1000, 180, "pms_status == 4", "urn:walkthru-earth:property:pm10_cf1_mass_concentration", "Plantower", "PMS5003T")
SEM(pm1_atmospheric_ug_m3, "ug/m3", physicalMeasurement, "mass_concentration_of_pm1_ambient_aerosol_particles_in_air", mean, 0, 1000, 180, "pms_status == 4", "http://vocab.nerc.ac.uk/standard_name/mass_concentration_of_pm1_ambient_aerosol_particles_in_air/", "Plantower", "PMS5003T")
SEM(pm25_atmospheric_ug_m3, "ug/m3", physicalMeasurement, "mass_concentration_of_pm2p5_ambient_aerosol_particles_in_air", mean, 0, 1000, 180, "pms_status == 4", "http://vocab.nerc.ac.uk/collection/P07/current/YUDG9DXK/", "Plantower", "PMS5003T")
SEM(pm10_atmospheric_ug_m3, "ug/m3", physicalMeasurement, "mass_concentration_of_pm10_ambient_aerosol_particles_in_air", mean, 0, 1000, 180, "pms_status == 4", "http://vocab.nerc.ac.uk/standard_name/mass_concentration_of_pm10_ambient_aerosol_particles_in_air/", "Plantower", "PMS5003T")
SEM(particles_gt03_per_01l, "{particles}/dL", physicalMeasurement, "", mean, 0, 65535, 180, "pms_status == 4", "urn:walkthru-earth:property:particle_count_gt_0p3um", "Plantower", "PMS5003T")
SEM(particles_gt05_per_01l, "{particles}/dL", physicalMeasurement, "", mean, 0, 65535, 180, "pms_status == 4", "urn:walkthru-earth:property:particle_count_gt_0p5um", "Plantower", "PMS5003T")
SEM(particles_gt10_per_01l, "{particles}/dL", physicalMeasurement, "", mean, 0, 65535, 180, "pms_status == 4", "urn:walkthru-earth:property:particle_count_gt_1um", "Plantower", "PMS5003T")
SEM(particles_gt25_per_01l, "{particles}/dL", physicalMeasurement, "", mean, 0, 65535, 180, "pms_status == 4", "urn:walkthru-earth:property:particle_count_gt_2p5um", "Plantower", "PMS5003T")
SEM(sensor_serial_code, "1", referenceInformation, "", mode, , , 0, "finite", "urn:walkthru-earth:property:particle_sensor_serial_code", "Plantower", "PMS5003T")
SEM(pm25_batch_candidate_ug_m3, "ug/m3", modelResult, "", mean, 0, 1000, 180, "pm25_batch_candidate_status == 3", "urn:walkthru-earth:property:pm25_batch_candidate_mass_concentration", "", "")
SEM(pm25_batch_candidate_status, "{code}", qualityInformation, "", mode, , , 0, "", "urn:walkthru-earth:property:pm25_batch_candidate_status", "", "")
SEM(ambient_temperature_c, "Cel", physicalMeasurement, "air_temperature", mean, -10, 60, 0, "finite", "http://vocab.nerc.ac.uk/collection/P07/current/CFSN0023/", "Plantower", "PMS5003T")
SEM(relative_humidity_percent, "%", physicalMeasurement, "relative_humidity", mean, 0, 100, 0, "finite", "http://vocab.nerc.ac.uk/collection/P07/current/CFSN0413/", "Plantower", "PMS5003T")
SEM(pms_firmware, "{code}", referenceInformation, "", mode, , , 0, "finite", "urn:walkthru-earth:property:particle_sensor_firmware_version", "Plantower", "PMS5003T")
SEM(pms_error, "{code}", qualityInformation, "", mode, , , 0, "finite", "urn:walkthru-earth:property:particle_sensor_error_code", "Plantower", "PMS5003T")
SEM(pms_frames, "{count}", qualityInformation, "", point, , , 0, "", "urn:walkthru-earth:property:particle_sensor_frame_count", "Plantower", "PMS5003T")
SEM(pms_checksum_errors, "{count}", qualityInformation, "", point, , , 0, "", "urn:walkthru-earth:property:particle_sensor_checksum_error_count", "Plantower", "PMS5003T")
SEM(pms_length_errors, "{count}", qualityInformation, "", point, , , 0, "", "urn:walkthru-earth:property:particle_sensor_length_error_count", "Plantower", "PMS5003T")
SEM(gauge_status, "{code}", qualityInformation, "", mode, , , 0, "", "urn:walkthru-earth:property:battery_gauge_status", "Analog Devices", "MAX17048")
SEM(battery_mv, "mV", auxiliaryInformation, "", mean, 2500, 4500, 0, "finite", "urn:walkthru-earth:property:battery_voltage", "Analog Devices", "MAX17048")
SEM(battery_percent, "%", auxiliaryInformation, "", point, 0, 100, 0, "", "urn:walkthru-earth:property:battery_state_of_charge", "Analog Devices", "MAX17048")
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
SEM(clock_epoch, "{count}", referenceInformation, "", point, , , 0, "", "urn:walkthru-earth:property:clock_epoch", "", "")
SEM(collection_completed_mono_us, "us", referenceInformation, "", point, , , 0, "", "urn:walkthru-earth:property:collection_completed_time_since_boot", "", "")
SEM(pms_received_mono_us, "us", referenceInformation, "", point, , , 0, "finite", "urn:walkthru-earth:property:particle_sensor_frame_received_time_since_boot", "Plantower", "PMS5003T")
SEM(clock_anchor_mono_us, "us", referenceInformation, "", point, , , 0, "finite", "urn:walkthru-earth:property:clock_anchor_time_since_boot", "", "")
SEM(clock_anchor_utc_us, "us", referenceInformation, "", point, , , 0, "finite", "urn:walkthru-earth:property:clock_anchor_utc_time", "", "")
SEM(h3_cell_id, "{h3_index}", referenceInformation, "", mode, , , 0, "finite", "urn:walkthru-earth:property:h3_cell_index", "", "")
SEM(h3_resolution, "1", referenceInformation, "", mode, 0, 15, 0, "finite", "urn:walkthru-earth:property:h3_resolution", "", "")
SEM(location_source, "{code}", qualityInformation, "", mode, , , 0, "finite", "urn:walkthru-earth:property:location_source", "", "")
SEM(geometry, "", coordinate, "", mode, , , 0, "", "urn:walkthru-earth:property:h3_cell_center_point", "", "")
SEM(battery_soc_raw_percent, "%", auxiliaryInformation, "", point, 0, 120, 0, "finite", "urn:walkthru-earth:property:battery_state_of_charge_raw", "Analog Devices", "MAX17048")
SEM(battery_charge_rate_percent_per_h, "%/h", auxiliaryInformation, "", mean, , , 0, "finite", "urn:walkthru-earth:property:battery_charge_rate", "Analog Devices", "MAX17048")
SEM(gauge_status_flags, "{bitmask}", qualityInformation, "", mode, , , 0, "finite", "urn:walkthru-earth:property:battery_gauge_alert_flags", "Analog Devices", "MAX17048")
SEM(gauge_hibernating, "{boolean}", auxiliaryInformation, "", mode, , , 0, "finite", "urn:walkthru-earth:property:battery_gauge_hibernating", "Analog Devices", "MAX17048")
SEM(battery_health_state, "{code}", auxiliaryInformation, "", mode, , , 0, "finite", "urn:walkthru-earth:property:battery_health_state", "", "")
SEM(battery_minutes_to_empty, "min", auxiliaryInformation, "", point, 0, 10080, 0, "finite", "urn:walkthru-earth:property:battery_time_to_empty", "", "")
