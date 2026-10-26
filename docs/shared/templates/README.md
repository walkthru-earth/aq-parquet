# Published board templates

Each board publishes its stac-opensensor v0.1.0 board template here, one file
per board for the current dictionary digest (`dict`).

| Board | Template |
| --- | --- |
| M5Stack CoreS3 | `m5stack-cores3/<dict sha256>.json` |
| Waveshare ESP32-S3 SIM7670G | `waveshare-esp32-s3-sim7670g/<dict sha256>.json` |

[`index.json`](index.json) maps each `dict` to its board, the firmware schema
name, the template `href` (relative to this folder) and `desc`.

## Byte exact

A template file holds exactly the `kTemplateJson` bytes the board embeds from
its generated `telemetry_template.h`. It is minified ASCII JSON with no final
newline. `desc` is the SHA-256 of these exact bytes, so the SHA-256 of the
file equals `desc` in the board `telemetry_descriptor_digest.h`. Never
reformat, pretty print or re-serialize a file, and never add a newline.
`.gitattributes` marks these files `-text` so git never touches their line
endings.

Only the current `dict` of each board is published. Older digests are dropped
(no backward compatibility, owner decision 14). Code values live in each
template as `classification:classes`, generated from `telemetry_classes.inc`.

## Mobile

The app copies these files verbatim into its bundled templates. Its template
index resolves a board by `desc` first, then by `dict`. Hash the bytes as
copied, never a parsed and re-serialized copy.

## Regenerate

The files come from the board headers. After a change to `telemetry_fields.inc`,
`telemetry_semantics.inc` or `telemetry_classes.inc`, one command per board
regenerates the headers and the published files together.

```sh
pixi run python tools/test_telemetry_contract.py --write-template
pixi run python tools/test_waveshare_contract.py --write-template
```

Each command rewrites the headers of its board, then publishes the current
headers of every board, removes stale templates and rewrites `index.json`.
`pixi run published-templates-test` is the gate, also run by `pixi run gate`.
