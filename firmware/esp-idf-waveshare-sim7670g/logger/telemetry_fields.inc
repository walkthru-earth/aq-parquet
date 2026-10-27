// FIELD(name, physical_type, procedure, unit, validity, live_key, decimals).
// live_key is the short LIVE JSON key, or "" for fields that stay out of
// LIVE. decimals is the number of digits after the point when a Float prints,
// chosen from the sensor resolution, and 0 for integer fields. Both are part
// of the digest, so the app keeps one LIVE profile per dictionary SHA-256.
// Parquet/Iceberg field_id = 1-based order of the FIELD entries below. The
// Thing release (#83) is the baseline. From it, append new fields only and
// never reorder, remove or reuse a position (owner answer to bridge question
// 36, 2026-10-26).
// The property id of each field comes from telemetry_semantics.inc, board
// neutral, looked up by name (#99).
// Float decimals. PMS5003T temperature and humidity step 0.1, so 1. MAX17048
// state of charge steps about 1/256 percent, 2 is enough, and charge rate
// 0.208 percent per hour, 1 is enough. The batch PM2.5 candidate keeps 1.
// PMS5003T has FOUR count bins; bytes 24/26 carry temperature and humidity.
FIELD(schema_version, Int32, logger, "1", always, "", 0)
FIELD(device_id, Int64, identity, "1", always, "", 0)
FIELD(boot_id_hi, Int64, identity, "1", always, "", 0)
FIELD(boot_id_lo, Int64, identity, "1", always, "", 0)
FIELD(sequence, Int64, logger, "count", always, "seq", 0)
FIELD(monotonic_us, Int64, clock, "us", always, "mono", 0)
FIELD(scheduled_us, Int64, clock, "us", always, "", 0)
FIELD(sample_jitter_us, Int64, clock, "us", always, "", 0)
FIELD(event_time_utc_us, Int64, clock, "us_since_unix_epoch", clock_anchored, "utc", 0)
FIELD(clock_status, Int32, clock, "code", always, "", 0)
FIELD(pms_status, Int32, pms5003t, "code", always, "pms", 0)
FIELD(pms_age_ms, Int64, pms5003t, "ms", pms_present, "", 0)
FIELD(pm1_cf1_ug_m3, Int32, pms5003t, "ug/m3", pms_valid, "c1", 0)
FIELD(pm25_cf1_ug_m3, Int32, pms5003t, "ug/m3", pms_valid, "c25", 0)
FIELD(pm10_cf1_ug_m3, Int32, pms5003t, "ug/m3", pms_valid, "c10", 0)
FIELD(pm1_atmospheric_ug_m3, Int32, pms5003t, "ug/m3", pms_valid, "pm1", 0)
FIELD(pm25_atmospheric_ug_m3, Int32, pms5003t, "ug/m3", pms_valid, "pm25", 0)
FIELD(pm10_atmospheric_ug_m3, Int32, pms5003t, "ug/m3", pms_valid, "pm10", 0)
FIELD(particles_gt03_per_01l, Int32, pms5003t, "count/0.1L", pms_valid, "n03", 0)
FIELD(particles_gt05_per_01l, Int32, pms5003t, "count/0.1L", pms_valid, "n05", 0)
FIELD(particles_gt10_per_01l, Int32, pms5003t, "count/0.1L", pms_valid, "n1", 0)
FIELD(particles_gt25_per_01l, Int32, pms5003t, "count/0.1L", pms_valid, "n25", 0)
FIELD(sensor_serial_code, Int64, owner_provisioned_identity, "1", exact_pms5003t_serial, "", 0)
FIELD(pm25_batch_candidate_ug_m3, Float, airgradient_20260408_count_candidate_v1, "ug/m3", owner_opt_in_exact_batch_pms_valid_not_cairo_validated, "", 1)
FIELD(pm25_batch_candidate_status, Int32, batch_candidate_gate, "code", always, "", 0)
FIELD(ambient_temperature_c, Float, pms5003t, "degC", pms_valid, "t", 1)
FIELD(relative_humidity_percent, Float, pms5003t, "%", pms_valid_range_0_100, "rh", 1)
FIELD(pms_firmware, Int32, pms5003t, "code", pms_present, "", 0)
FIELD(pms_error, Int32, pms5003t, "code", pms_present, "", 0)
FIELD(pms_frames, Int64, pms5003t, "count", always, "", 0)
FIELD(pms_checksum_errors, Int64, pms5003t, "count", always, "", 0)
FIELD(pms_length_errors, Int64, pms5003t, "count", always, "", 0)
FIELD(gauge_status, Int32, max17048, "code", always, "", 0)
FIELD(battery_mv, Int32, max17048, "mV", battery_installed_vcell_read_ok, "bat", 0)
FIELD(battery_percent, Int32, max17048, "%", battery_installed_soc_read_ok_range_0_100, "pct", 0)
FIELD(heap_free_bytes, Int64, esp_runtime, "byte", always, "", 0)
FIELD(heap_min_free_bytes, Int64, esp_runtime, "byte", always, "", 0)
FIELD(psram_free_bytes, Int64, esp_runtime, "byte", always, "", 0)
FIELD(sd_total_bytes, Int64, storage, "byte", sd_mounted_cached, "", 0)
FIELD(sd_used_bytes, Int64, storage, "byte", sd_mounted_cached, "", 0)
FIELD(rows_dropped, Int64, logger, "count", always, "", 0)
FIELD(sample_deadlines_missed, Int64, logger, "count", always, "", 0)
FIELD(storage_errors, Int64, storage, "count", always, "", 0)
FIELD(files_finalized, Int64, storage, "count", always, "", 0)
FIELD(last_write_us, Int64, storage, "us", always, "", 0)
FIELD(last_sync_us, Int64, storage, "us", always, "", 0)
FIELD(queue_high_water, Int32, logger, "count", always, "", 0)
FIELD(clock_epoch, Int32, clock, "count", always, "", 0)
FIELD(collection_completed_mono_us, Int64, clock, "us", always, "", 0)
FIELD(pms_received_mono_us, Int64, pms5003t, "us", pms_present, "", 0)
FIELD(clock_anchor_mono_us, Int64, clock, "us", clock_anchored, "", 0)
FIELD(clock_anchor_utc_us, Int64, clock, "us_since_unix_epoch", clock_anchored, "", 0)

// Optional owner-provisioned H3 cell. location_source says where it came from
// and geometry is a native Parquet GEOMETRY 2D WKB Point at the cell centre,
// never the raw fix. All four are null when there is no location.
FIELD(h3_cell_id, Int64, h3_grid, "h3_index", location_present, "", 0)
FIELD(h3_resolution, Int32, h3_grid, "resolution_0_15", location_present, "", 0)
FIELD(location_source, Int32, h3_grid, "uint8_code", location_present, "", 0)
FIELD(geometry, ByteArray, h3_grid_center, "wkb_point_crs84", location_present, "", 0)

// Schema v5: MAX17048 diagnostics and the shared battery health classifier.
FIELD(battery_soc_raw_percent, Float, max17048, "%", battery_installed_soc_read_ok_uncapped, "", 2)
FIELD(battery_charge_rate_percent_per_h, Float, max17048, "%/h", battery_installed_crate_read_ok, "", 1)
FIELD(gauge_status_flags, Int32, max17048, "bitmask", battery_installed_status_read_ok, "", 0)
FIELD(gauge_hibernating, Int32, max17048, "boolean", battery_installed_mode_read_ok, "", 0)
FIELD(battery_health_state, Int32, battery_health, "code", battery_installed, "bstate", 0)
FIELD(battery_minutes_to_empty, Int32, battery_health, "min", battery_soc_trusted_discharging_within_7_days, "", 0)
