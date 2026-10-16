# Mobile app migration notes

The firmware does not keep backward compatibility with older file schemas or
protocol shapes. Each section below records one breaking change the Android and
iOS apps must handle. It says what changed, why, and what the app must do.
Newest changes come first. Files already on a phone or SD card are never
rewritten, so the app must keep reading the older shapes it has stored.

## UTC columns in microseconds and Parquet field ids (CoreS3 v6.12, Waveshare v2.6)

**What changed.**

- `event_time_utc_ns` is now `event_time_utc_us` and `clock_anchor_utc_ns` is
  now `clock_anchor_utc_us`. Values are INT64 microseconds since the Unix
  epoch, annotated `TIMESTAMP(MICROS, isAdjustedToUTC=true)`. The dictionary
  unit is `us_since_unix_epoch` (was `ns_since_unix_epoch`).
- Every leaf column now carries a Parquet `field_id`. The id is the column's
  1-based position in the board's `telemetry_fields.inc`, and dictionaries are
  append only, so an id never moves to another column.
- CoreS3 writes schema `cores3-telemetry-v5` (row value 5), dictionary
  `cores3-telemetry-v4`, firmware `idf-cores3-parquet-v6.12`. Waveshare writes
  schema and dictionary `waveshare-sim7670g-telemetry-v4` (row value 4),
  firmware `idf-waveshare-parquet-v2.6`. Both dictionary SHA-256 values change.
- Unchanged. The `utc` key in the LIVE document is still nanoseconds. The
  footer key `reconciliation_anchor_utc_ns` is still nanoseconds. Column count,
  column order and all other names and types are unchanged.

**Why.** Apache Iceberg v2 has no nanosecond timestamp type. A
`TIMESTAMP(NANOS)` column only maps to Iceberg v3 `timestamptz_ns`, which most
engines cannot read yet. Iceberg also identifies columns by field id, not by
name. With MICROS and field ids the phone or host can register device files in
an Iceberg v2 table without rewriting them. The firmware clock already had
microsecond resolution, so no precision is lost.

**What the app must do.**

- Read the UTC instant from `event_time_utc_us` when present and fall back to
  `event_time_utc_ns` for older files. Convert by unit, never by name alone.
  Check the Parquet logical type (`MICROS` or `NANOS`).
- Accept schema versions 5 (CoreS3) and 4 (Waveshare) and the new dictionary
  digests. Do not reject a file only because its schema string is newer.
- When building an Iceberg manifest or schema, use each leaf's `field_id` as the
  Iceberg field id. Older files have no ids. Map them by name to the same ids,
  or keep them out of the Iceberg table.
- When reconciling unsynchronized files from footer anchors, compute
  microseconds as `reconciliation_anchor_utc_ns / 1000 + (monotonic_us -
  reconciliation_anchor_mono_us)`.
- Keep treating LIVE `utc` as nanoseconds.
