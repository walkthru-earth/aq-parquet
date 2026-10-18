# esp-nimble host patch for ESP-IDF v6.1

ESP-IDF v6.1 pins espressif/esp-nimble at `139cada`. Its `ble_hs_hci_cmd_tx()`
can crash the host after an HCI command times out. A late ack leaves the
command semaphore at 1, the reset's HCI_Reset takes that stale count, and
`ble_hs_hci_process_ack()` reads a NULL ack (LoadProhibited, issue #51).

Upstream fixed this in espressif/esp-nimble on `nimble-1.10.0-idf`.

- `d7061976e9a10b18ddd41ecdf118f039b8d34477` frees a stale ack before sending.
- `79316193f942060e9c79200ddd032157453b041d` drains the semaphore before sending.

Neither is in ESP-IDF `release/v6.1` or the `v6.1` tag.
[ble_hs_hci-stale-ack.patch](ble_hs_hci-stale-ack.patch) backports both to the
pinned file. The SDK checkout stays untouched.
[`firmware/cmake/aq_nimble_patches.cmake`](../../../cmake/aq_nimble_patches.cmake)
copies the upstream file into the build directory, applies the patch with no
fuzz and compiles the copy in place of the SDK file. Images without NimBLE
skip the step.

| File | SHA-256 |
| --- | --- |
| upstream `nimble/host/src/ble_hs_hci.c` (pinned in the CMake step) | 576e8582bf25dd56170ce1d47d5c64ccddf1fc5facd7dcfa818dc19629cbd7b1 |
| ble_hs_hci-stale-ack.patch | 8f6afd6356400802f2c73b8db12709a63962a2cacfcf095e48a061bfb295b164 |
| LICENSE | 0deb15bc7df50b1d009c644b008f92e1062909e9521328d337c5fad3a5d4921c |
| NOTICE | b838c4733a5020cd8e2bafbe70842acd8c8dc7816012965b77d10c1f4c0727f6 |

The patch and the patched file are Apache-2.0, as is upstream NimBLE.
[LICENSE](LICENSE) and [NOTICE](NOTICE) are copied from the pinned SDK tree.
These files are not relicensed under the repository's CC BY license.

Configure fails when the upstream file no longer matches the pin. After an SDK
upgrade, check whether the SDK carries `79316193f9`. If it does, delete this
directory, the CMake step and its calls. If not, rebase the patch and update
the pin and this table.
