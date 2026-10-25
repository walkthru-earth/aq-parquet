# STAC metadata contract research

Research snapshot from 2026-10-25 at commit `2b49114`. Nothing here is implemented. It records why and how the firmware could publish a STAC description of its columns, so the mobile app and host tools stop being air quality only.

| File | Content |
| --- | --- |
| [firmware-audit.md](firmware-audit.md) | Where per-column metadata lives, what reaches the phone, footer and LIVE limits, a STAC emission proposal with flash and RAM cost, and which AQ names are wire contract |
| [firmware-audit-review.md](firmware-audit-review.md) | Independent line-by-line check of the audit. Its corrections win where the two disagree, for example footer slot use, descriptor size, request per chunk transfer and unit systems |

The app-side design is `docs/superpowers/specs/2026-10-25-sensor-agnostic-stac-contract.md` in walkthru-earth/opensensor-space-mobile, tracked by walkthru-earth/opensensor-space-mobile#182. Firmware work is tracked by epic #83 and issues #84 to #93.

When any part lands, move the settled contract into `table-and-observation-model.md` and `ble-sync-protocol.md`, and keep this folder as history.

## Field names are superseded

The two audits are dated and propose draft field names. Their findings still stand, but the names were replaced by the released extension [stac-opensensor v0.1.0](https://github.com/walkthru-earth/stac-opensensor). Read the audits for evidence and use the extension for names.

v0.1.0 splits the metadata into three layers, as its [PROFILE.md](https://github.com/walkthru-earth/stac-opensensor/blob/main/PROFILE.md) defines.

1. Board template. The firmware build generates one per schema, plain JSON with `"type": "OpenSensorTemplate"`, not STAC and not a profile Collection. It holds the public columns in `item_assets.data.table:columns` (core `unit`, core `roles` and the `opensensor:` column fields, no `title` or `opensensor:scale`), `instruments`, `opensensor:sample_interval_s`, and a `firmware` block with `schema`, `proto_min`, `dict`, `live_keys` and `unit_labels`. Its SHA-256 is the `desc` digest and `GET_DESCRIPTOR` streams it. The dictionary digest reaches it as `file:checksum` on a `describedby` link to `telemetry_fields.inc`. There is no `cube:dimensions`.
2. Station Collection. The catalog publisher copies the template columns and adds titles, scales, extent, license and a second `describedby` link to the template with `file:checksum` set to `1220` plus `desc`.
3. App profile. `primary`, `hero`, `family`, `display_step` and column labels live in an app file keyed by the dictionary digest, outside any published schema.

| Draft name in the audits | v0.1.0 |
| --- | --- |
| `opensensor:ucum`, or a UDUNITS and UCUM pair | Core `unit` with a UCUM code only |
| `opensensor:unit_label` | Board template `firmware.unit_labels` |
| `opensensor:live_key` | Board template `firmware.live_keys` |
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
| Footer `stac:collection_href`, `stac:descriptor_sha256`, `urn:opensensor:collection:` | `opensensor:template_href`, `opensensor:template_sha256`, `urn:opensensor:template:` |
| `opensensor:field_id`, `opensensor:procedure`, `opensensor:property` | Unchanged |

`opensensor:settle_s` and `opensensor:derived` are new in v0.1.0. Upstream proposals may move some fields later, see stac-extensions/table#16 (`field_id`), table#17 (common metadata in columns), classification#55 (classes in table columns) and cf#11 (CF fields in table columns).
