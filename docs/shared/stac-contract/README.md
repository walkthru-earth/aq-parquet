# STAC metadata contract research

Research snapshot from 2026-10-25 at commit `2b49114`. Nothing here is implemented. It records why and how the firmware could publish a STAC description of its columns, so the mobile app and host tools stop being air quality only.

| File | Content |
| --- | --- |
| [firmware-audit.md](firmware-audit.md) | Where per-column metadata lives, what reaches the phone, footer and LIVE limits, a STAC emission proposal with flash and RAM cost, and which AQ names are wire contract |
| [firmware-audit-review.md](firmware-audit-review.md) | Independent line by line check of the audit. Its corrections win where the two disagree, for example footer slot use, descriptor size, request per chunk transfer and unit systems |

The app side design is `docs/superpowers/specs/2026-10-25-sensor-agnostic-stac-contract.md` in walkthru-earth/opensensor-space-mobile, tracked by walkthru-earth/opensensor-space-mobile#182. Firmware work is tracked by issues in this repo that link back to that epic.

When any part lands, move the settled contract into `table-and-observation-model.md` and `ble-sync-protocol.md`, and keep this folder as history.
