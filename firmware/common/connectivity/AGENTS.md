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
- Preserve the ESP32-S3 `TWO` universal-MAC policy from both prior board images:
  Bluetooth uses the factory base + 1. IDF's `FOUR` default changes it to base +
  2, preventing the stored local IRK lookup and breaking phone identity matches.
  Do not change this policy or clear keys to silence migration warnings.
- Native Wi-Fi uses RAM driver storage; `aqcfg` alone persists credentials.
  Event callbacks publish state only; the LAN task owns reconnects, scans,
  mDNS and sockets. Use `esp_netif_sntp` for safe TCP/IP marshaling and publish
  UTC only on a successful sync callback. Never log a PSK or LAN token.
- Radio ownership belongs to this module once started. No second Wi-Fi/NimBLE initializer in a consuming trial. Configuration/logging live in the opt-in AQRuntime component; pure encoding/helpers live in AQCommon.
- Every session close carries a typed `ble::DisconnectReason` (`disconnect.h`). Record it once through `record_disconnect`, and send LAN `BYE` before closing. New reasons append codes and update the protocol doc, `tools/ble_sync.py` and the DIAG counter count together.
- LAN transmit goes through each peer's `aqsync::FrameRing`. Only the LAN task writes sockets, non-blocking. Never add a blocking send or a socket write from the worker. The worker uses `lan::deliver` with a wait-step hook that services samples and stops data frames on ABORT. Ring size, slow-peer timeout and queue depths come from `aqsync::ServiceConfig`, never a board `#if`.
- BLE `response` frames go through notify credits (`ble_policy.h`). Only the NimBLE host task notifies. The worker uses `ble::deliver` with the same wait-step hooks as LAN. A credit returns when the host accepts the frame (NOTIFY_TX fires inside `ble_gatts_notify_custom`), and a refused frame is retried by a host callout, never by a worker sleep. Every queue clear starts a new epoch so a late publish is rejected, a Response unsubscribe closes the stream and a resubscribe bumps the generation. A slow-peer terminate that gets `BLE_HS_EALREADY` keeps the pending reason and stops retrying. Advertising, link parameter requests and credit limits come from `ble::Options` in `aqsync::ServiceConfig`, never a board `#if`.
- Transports stamp every request with `aqsync::abort::stamp` before queueing. Long worker loops check `abort::pending`, end with ERROR 16 and call `note_stopped`.
- Shared source compilation and host codec tests are distinct from on-board radio/coexistence measurements. Do not relabel CoreS3 bench results as another board's results.
