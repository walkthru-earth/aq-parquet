# Shared ESP32-S3 connectivity

This opt-in Arduino library implements the shared BLE/LAN transport contract. It uses Arduino-ESP32 and NimBLE, and is validated with the exact versions in each consuming trial's lockfile. UART-only trials do not consume it.

- Never include a board trial or its telemetry logger. Firmware passes stable identity strings and a request handler before tasks/radios start.
- A request handler must copy/enqueue synchronously without filesystem, display, sensor or power access. BLE and LAN invoke it concurrently; queue saturation returns false and becomes a busy response on the originating link.
- The board storage worker calls shared `ArchiveSession` and common configuration/log/token control handlers, then executes runtime-specific clock/flush/reboot/status operations. It publishes status/live/advertising snapshots. Keep session generation, frame limits, authentication, token secrecy, and callback threading behavior intact.
- With pinned NimBLE 2.5.1, use the server onPassKeyDisplay callback for both
  random and fixed pairing. Setting a nondefault static security passkey
  bypasses that callback and breaks shared pairing/UI state. The callback
  returns the configured per-device PIN for fixed mode without logging it.
- Radio ownership belongs to this module once started. No second Wi-Fi/NimBLE initializer in a consuming trial. Configuration/logging live in the opt-in AQRuntime library; pure encoding/helpers live in AQCommon.
- Shared source compilation and host codec tests are distinct from on-board radio/coexistence measurements. Do not relabel CoreS3 bench results as another board's results.
