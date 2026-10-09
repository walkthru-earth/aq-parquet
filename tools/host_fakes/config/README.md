# Configuration test platform

These headers supply the small platform surface used by the **real** shared
configuration and log implementations. They do not reimplement settings parsing,
pairing policy, JSON generation, log framing or control handlers.

`nvs.h` models native ESP-IDF NVS types, staged values, explicit per-key commits
and string lengths including the terminating NUL. Tests inject namespace-open,
individual key-write and commit failures and verify stored value types. They
establish error reporting and unchanged runtime settings on failure; they do not
establish transactional NVS writes or power-loss safety.

The deterministic random source supplies test fixtures, never production entropy.
The host mutex models mutual exclusion, not interrupt latency or radio scheduling.
The fake native console sink keeps synthetic test logs out of terminal output; the real
`DebugLog` ring remains exercised. Control tests replace only BLE/LAN observations
and action calls. Real pairing, Wi-Fi connections and storage require board tests.

Run `pixi run config-test --sanitize` and `pixi run control-sync-test --sanitize`.
