# Shared ESP32-S3 connectivity

These opt-in native ESP-IDF components implement the shared BLE/LAN transport
contract. ESP-IDF 6.1 supplies NimBLE, Wi-Fi, events, netif and SNTP; managed
`espressif/mdns` is pinned to 1.14.0. No Arduino APIs or NimBLE-Arduino wrapper.
Wire behavior belongs to `docs/shared/ble-sync-protocol.md`; hardware evidence
remains tied to each board image and is historical until native readback.

- Never include a board trial or its telemetry logger. Firmware passes stable identity strings and a request handler before tasks/radios start.
- A request handler must copy/enqueue synchronously without filesystem, display, sensor or power access. BLE and LAN invoke it concurrently; queue saturation returns false and becomes a busy response on the originating link.
- The board storage worker calls shared `ArchiveSession` and common configuration/log/token control handlers, then executes runtime-specific clock/flush/reboot/status operations. It publishes status/live/advertising snapshots. Keep session generation, frame limits, authentication, token secrecy, and callback threading behavior intact.
- Native NimBLE owns pairing, bonding and GATT/GAP. Preserve encrypted and
  authenticated characteristic permissions in random/fixed modes, callback
  delivery of per-attempt random or per-device fixed PINs, the public address,
  UUIDs, MTU bounds, long reads and ordered responses. Existing Arduino-era
  bonds are not assumed compatible; qualify bonded reconnect or re-pairing on
  hardware without erasing station/config NVS.
- Native Wi-Fi uses RAM driver storage; `aqcfg` alone persists credentials.
  Event callbacks publish state only; the LAN task owns reconnects, scans,
  mDNS and sockets. Use `esp_netif_sntp` for safe TCP/IP marshaling and publish
  UTC only on a successful sync callback. Never log a PSK or LAN token.
- Radio ownership belongs to this module once started. No second Wi-Fi/NimBLE initializer in a consuming trial. Configuration/logging live in the opt-in AQRuntime component; pure encoding/helpers live in AQCommon.
- Shared source compilation and host codec tests are distinct from on-board radio/coexistence measurements. Do not relabel CoreS3 bench results as another board's results.
