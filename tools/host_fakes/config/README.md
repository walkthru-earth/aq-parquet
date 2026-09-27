# Configuration test platform

These headers supply the small platform surface used by the **real** shared
configuration and log implementations. They do not reimplement settings parsing,
pairing policy, JSON generation, log framing or control handlers.

`Preferences.h` models the return values observed in the pinned Arduino-ESP32
3.3.11 SDK source: `putString` excludes the terminating NUL, `getString` includes
it, and each `put` commits separately. Tests inject namespace-open and individual
key-write failures. They establish error reporting and unchanged runtime settings
on failure; they do not establish transactional NVS writes or power-loss safety.

The deterministic random source supplies test fixtures, never production entropy.
The host mutex models mutual exclusion, not interrupt latency or radio scheduling.
The fake serial sink keeps synthetic test logs out of terminal output; the real
`DebugLog` ring remains exercised. Control tests replace only BLE/LAN observations
and action calls. Real pairing, Wi-Fi connections and storage require board tests.

Run `pixi run config-test --sanitize` and `pixi run control-sync-test --sanitize`.
