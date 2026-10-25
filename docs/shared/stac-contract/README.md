# STAC metadata contract research

Research snapshot from 2026-10-25 at commit `2b49114`. Nothing here is implemented. It records why and how the firmware could publish a STAC description of its columns, so the mobile app and host tools stop being air quality only.

| File | Content |
| --- | --- |
| [firmware-audit.md](firmware-audit.md) | Where per-column metadata lives, what reaches the phone, footer and LIVE limits, a STAC emission proposal with flash and RAM cost, and which AQ names are wire contract |
| [firmware-audit-review.md](firmware-audit-review.md) | Independent line by line check of the audit. Its corrections win where the two disagree, for example footer slot use, descriptor size, request per chunk transfer and unit systems |

The app side design is `docs/superpowers/specs/2026-10-25-sensor-agnostic-stac-contract.md` in walkthru-earth/opensensor-space-mobile, tracked by walkthru-earth/opensensor-space-mobile#182. Firmware work is tracked by epic #83 and issues #84 to #93.

When any part lands, move the settled contract into `table-and-observation-model.md` and `ble-sync-protocol.md`, and keep this folder as history.

## Field names are superseded

The two audits are dated and propose draft field names. Their findings still stand, but the names were replaced by the released extension [stac-opensensor v0.1.0](https://github.com/walkthru-earth/stac-opensensor). Read the audits for evidence and use the extension for names.

v0.1.0 splits the metadata into three layers.

1. Public STAC fields. Each station Collection carries its columns in `item_assets.data.table:columns`, with core `unit`, core `roles` and the `opensensor:` column fields. There is no world extent profile Collection per board schema and no `cube:dimensions`.
2. Firmware descriptor facts. `live_key`, `unit_label` and `proto_min` stay in the descriptor the phone reads over BLE or LAN. The dictionary digest reaches STAC as `file:checksum` on a `describedby` link to `telemetry_fields.inc`.
3. App profile. `primary`, `hero`, `family`, `display_step` and column labels live in an app file keyed by the dictionary digest, outside any published schema.

| Draft name in the audits | v0.1.0 |
| --- | --- |
| `opensensor:ucum`, or a UDUNITS and UCUM pair | Core `unit` with a UCUM code only |
| `opensensor:unit_label` | Firmware descriptor `unit_label` |
| `opensensor:live_key` | Firmware descriptor `live_key` |
| `opensensor:validity` | `opensensor:valid_when`, a gate `{column, equals}` or `{column, finite: true}` |
| `opensensor:aggregation` (`instant`, `mean`, `counter`, `state`) | `opensensor:aggregate` with CF words `mean`, `minimum`, `maximum`, `sum`, `point`, `mode` |
| `opensensor:valid_range` | `opensensor:plausible_range`, values outside are flagged, not masked |
| `opensensor:display` scale and bands | `opensensor:scales` on the Collection, `opensensor:scale` on the column |
| `opensensor:labels`, `title_ar` | Column `title` plus app profile labels, scale step labels in `opensensor:scales` |
| `opensensor:precision` | App profile `display_step` |
| `opensensor:codes`, or a `bands` entry per code column | `classification:classes` in the column |
| `opensensor:sensor` (vendor, model) | `opensensor:procedure` on the column and core `instruments` on the Collection |
| `opensensor:station_id` on Items | One Collection per station UUID, one Item per day |
| `opensensor:calibration` | Not in v0.1.0 |
| Role values (measurement, quality, state and others) | Core `roles` with ACDD values `physicalMeasurement`, `auxiliaryInformation`, `qualityInformation`, `referenceInformation`, `coordinate` |
| `cube:dimensions`, `cube:variables` | Not used. Cadence is `opensensor:sample_interval_s` |
| Shared `urn:opensensor:property:<id>` | `opensensor:property` with a shared URI such as NERC P07 where one fits, otherwise a per board URI |
| `opensensor:field_id`, `opensensor:procedure`, `opensensor:property` | Unchanged |

`opensensor:settle_s` and `opensensor:derived` are new in v0.1.0. Upstream proposals may move some fields later, see stac-extensions/table#16 (`field_id`), table#17 (common metadata in columns), classification#55 (classes in table columns) and cf#11 (CF fields in table columns).
