# Published board templates

Each board publishes its stac-opensensor v0.1.0 board template here, one file
per board for the current dictionary digest (`dict`).

| Board | Template |
| --- | --- |
| M5Stack CoreS3 | `m5stack-cores3/<dict sha256>.json` |
| Waveshare ESP32-S3 SIM7670G | `waveshare-esp32-s3-sim7670g/<dict sha256>.json` |

[`index.json`](index.json) holds two maps. `templates` maps each `dict` to
its board, the firmware schema name, the template `href` (relative to this
folder) and `desc`. `by_desc` maps each `desc` to its board, `dict` and `href`,
so a reader that resolves by `desc` needs no scan.

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

The files come from the board headers. The command that writes them is the
board contract test with `--write-template`.

```sh
pixi run python tools/test_telemetry_contract.py --write-template
pixi run python tools/test_waveshare_contract.py --write-template
```

Each command rewrites `telemetry_template.h` and
`telemetry_descriptor_digest.h` of its board, then publishes the current
headers of every board, removes anything else under this folder but this
README and rewrites `index.json`.

A change to `telemetry_semantics.inc` or `telemetry_classes.inc` keeps `dict`,
so running the command for that board is enough. A change to
`telemetry_fields.inc` moves `dict`, and the command does not update the
dictionary digest or the LIVE fixture, so do these steps in order.

1. Edit `telemetry_fields.inc`, and bump `kDictionaryVersion` and `kFirmware`
   in `telemetry_contract.h` as the root `AGENTS.md` requires.
2. Set `kDictionarySha256` in `telemetry_dictionary_digest.h` to the output of
   `shasum -a 256 telemetry_fields.inc`. The contract test refuses a stale
   digest before it writes anything.
3. In `tools/fixtures/live_keys/<board>.json` (`cores3` or `waveshare`),
   replace the old digest entry with one for the new digest, holding the new
   firmware identity and its LIVE key to field map in field order.
4. Run the board command above. It publishes the template under the new
   `dict` name and removes the file for the old one.
5. Run the board contract test without the flag and
   `pixi run published-templates-test`, then `pixi run gate`.
6. Add a newest-first entry to [mobile-migration.md](../mobile-migration.md)
   with the new `dict` and `desc`.
