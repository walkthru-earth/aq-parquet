# OpenSensor Station Profile

| Item | Value |
| --- | --- |
| Title | OpenSensor Station Profile |
| Profile schema | <https://walkthru-earth.github.io/stac-opensensor/v0.1.0/profile.json> |
| Template schema | <https://walkthru-earth.github.io/stac-opensensor/v0.1.0/template.json> |
| Version | 0.1.0 |

The [extension](README.md) defines fields. This profile fixes how a whole station catalog uses them,
so any client can read any station without guessing.
It says which extensions each document must declare, which fields each column must carry and which vocabularies to use.

A document follows the profile when it lists the profile schema in `stac_extensions`.
STAC validators then check the profile like any other extension.

- [Full example](#full-example)
- [Profile schema](json-schema/profile.json)
- [Template schema](json-schema/template.json)

## Layers

One station is described by three layers. Each layer has one owner.

| Layer | Owner | File | Holds |
| --- | --- | --- | --- |
| Board template | Firmware build | `templates/<schema>.json` | Columns and their meaning, sample interval, instruments, LIVE keys, protocol floor |
| Station Collection | Catalog publisher | `<station>/collection.json` | Template columns plus titles, scales, location extent, license and links |
| App profile | Mobile app | Outside this repository | Card order, hero card, display step and column labels in each language |

The board template is plain JSON, not STAC. A firmware build generates it, its SHA-256 is the descriptor digest,
and a device can serve it over BLE or LAN. A station Collection is the template's columns with station facts added.
The app profile is the app's own choice and never goes into published STAC.

## Catalog layout

```text
catalog.json                  STAC Catalog, one child link per station
<station>/collection.json     station Collection, columns in item_assets
<station>/<yyyy>/<mm>/<dd>.json   day Item, one per reconciled day
<station>/<yyyy>/<mm>/<dd>.parquet  the day's samples
templates/<schema>.json       board template, cached forever by digest
```

## Station Collection

| Rule | Level |
| --- | --- |
| `stac_extensions` lists Table v1.3.0, the OpenSensor extension and this profile | MUST |
| `license`, `keywords` and `instruments` are set | MUST |
| `opensensor:sample_interval_s` is set | MUST |
| `item_assets.data` is a Parquet asset with role `data`, `table:primary_datetime` and `table:columns` | MUST |
| A `describedby` link points at the firmware dictionary with its `file:checksum` | MUST |
| A second `describedby` link points at the board template with its `file:checksum` | SHOULD |
| Columns equal the template's columns, plus only `title` and `opensensor:scale` | SHOULD |
| `opensensor:scales` holds every scale a column points at | MUST |

## Columns

Every column has `name`, `type` and exactly one value in `roles`. The role decides what else is required.

| Role | Meaning | Required beyond name, type and roles |
| --- | --- | --- |
| `physicalMeasurement` | Environmental reading | `unit`, `opensensor:property`, `opensensor:procedure`, `opensensor:field_id`, `opensensor:aggregate`, `opensensor:valid_when` |
| `auxiliaryInformation` | Board health such as battery or board temperature | `opensensor:procedure`, `opensensor:field_id`, `opensensor:aggregate` |
| `qualityInformation` | Status or code column | `opensensor:procedure`, `opensensor:field_id`, `opensensor:aggregate` |
| `modelResult` | Derived column | `unit`, `opensensor:derived`, `opensensor:aggregate` |
| `referenceInformation` | Identity or bookkeeping | Nothing more |
| `coordinate` | Time or position | Nothing more |

## Day Item

| Rule | Level |
| --- | --- |
| `stac_extensions` lists Table, Grid v1.1.0, Anonymized Location v1.0.0, the OpenSensor extension and this profile | MUST |
| `collection` names the station | MUST |
| Geometry is the owner's H3 cell polygon, never the device position | MUST |
| `grid:code` is `H3-` plus the 15 character cell | MUST |
| `anon:size` and `anon:warning` are set | MUST |
| `start_datetime` and `end_datetime` cover the UTC day, `datetime` is `null` | MUST |
| `assets.data` is the Parquet file with `table:row_count` | MUST |
| Measurement columns carry core `statistics` for the day | SHOULD |

## Conventions

These choices are fixed for every station.

| Topic | Choice |
| --- | --- |
| Units | UCUM codes in core `unit`, for example `ug/m3`, `Cel`, `%`, `[g]` and `{particles}/dL` |
| Firmware unit labels | Kept in the template as `firmware.unit_labels` when they differ from the UCUM code |
| Roles | ACDD `coverage_content_type` values, one per column |
| Aggregation | CF `cell_methods` words, `mean` for readings, `point` for levels such as battery, `mode` for codes |
| Code meanings | `classification:classes` on the column, `nodata` on the missing value |
| CF names | `cf:standard_name` only where the CF definition matches exactly, never on CF=1 particle columns |
| Property URIs | Board neutral. A NERC P07 term first, then `http://vocab.nerc.ac.uk/standard_name/<cf name>/` for an exact CF match, else `urn:walkthru-earth:property:<quantity>`. Board and sensor go in `opensensor:procedure`, never in the property |
| Column ids | `opensensor:field_id` is the 1-based FIELD order, append only |
| Time | `event_time_utc_us`, int64 microseconds since the Unix epoch, as `table:primary_datetime` |
| Location | H3 cell chosen by the owner, resolution 8 by default |
| Health scales | WHO 2021 guideline and interim targets, labels in English and Arabic, `color_hint` per step |
| Checksums | SHA-256 multihash in `file:checksum`, the `1220` prefix then 64 hex characters |
| License | CC BY 4.0 |
| Firmware facts | LIVE keys, protocol floor and unit labels stay in the template under `firmware` |
| App choices | Card order, hero card and display step stay in the app profile |

## Parquet footer

Every data file carries one key, `opensensor`, in its Parquet key-value metadata, modelled on the GeoParquet `geo` key.
The value is compact UTF-8 JSON that validates against [json-schema/footer.json](json-schema/footer.json).
No other footer key is part of the contract.
A file stands alone, so an indexer can build its station Collection and Item from the footer and the template it names.

| Field | Type | Description |
| --- | --- | --- |
| version | `"0.1.0"` | **REQUIRED**. Readers refuse an unknown major version |
| station | object | **REQUIRED**. `id`, `device_id`, `board`, and optional `deployment_id` |
| firmware | object | **REQUIRED**. `version`, `schema` and `boot_id` |
| dictionary | object | **REQUIRED**. `sha256` equals `firmware.dict` in the template, plus optional `version` and `uri` |
| template | object | **REQUIRED**. `href` and `sha256`, the SHA-256 of the exact template bytes, equal to the template link `file:checksum` without `1220` |
| acquisition | object | **REQUIRED**. `sample_interval_ms` and `time_semantics`, plus optional `config_id`, `config` and `calibration_id` |
| file | object | **REQUIRED**. `rotation_interval_s` at most 3600, `row_groups`, `row_group_rows_max`, `compression` and `purpose` |

Numbers are JSON numbers. The footer `sample_interval_ms` is the truth for that file, and the template value is only the default.
Code meanings, such as clock quality codes, belong in the template, not in the footer.
DuckDB reads it with `SELECT value::JSON FROM parquet_kv_metadata('file.parquet') WHERE key = 'opensensor'`.
Example [footers/cores3-telemetry-v5.json](footers/cores3-telemetry-v5.json).

## Board template

| Field | Type | Description |
| --- | --- | --- |
| type | `"OpenSensorTemplate"` | **REQUIRED** |
| template_version | `"0.1.0"` | **REQUIRED** |
| id | string | **REQUIRED**. Firmware schema name, such as `cores3-telemetry-v5` |
| title | string | Board and sensor names |
| keywords | \[string] | Search terms, copied to the station |
| instruments | \[string] | **REQUIRED**. Procedures, copied to the station |
| opensensor:sample_interval_s | number | **REQUIRED**. Seconds between samples |
| item_assets | object | **REQUIRED**. `data` with columns as in a station, without `title` or `opensensor:scale` |
| links | \[Link] | **REQUIRED**. `describedby` to the firmware dictionary with `file:checksum` |
| firmware | object | **REQUIRED**. See below |

| Firmware field | Type | Description |
| --- | --- | --- |
| schema | string | **REQUIRED**. Firmware schema name |
| proto_min | integer | **REQUIRED**. Lowest sync protocol the board speaks |
| dict | string | **REQUIRED**. SHA-256 of `telemetry_fields.inc`, as the device sends it |
| live_keys | Map\<string, string> | **REQUIRED**. Column name to LIVE JSON key |
| unit_labels | Map\<string, string> | Column name to firmware unit label, where it differs from `unit` |

## Full example

The examples describe a real M5Stack CoreS3 build, firmware schema `cores3-telemetry-v5`.
The template lists 11 of its 81 columns, one or more of each kind. A generated template lists them all.

| File | Layer |
| --- | --- |
| [templates/cores3-telemetry-v5.json](templates/cores3-telemetry-v5.json) | Board template, `dict` `e5060fb7…` |
| [examples/catalog.json](examples/catalog.json) | Root Catalog |
| [examples/collection.json](examples/collection.json) | Station Collection made from the template |
| [examples/item.json](examples/item.json) | Day Item |

`npm run check-profile` validates all of them and checks that the station's columns and checksums match the template.
