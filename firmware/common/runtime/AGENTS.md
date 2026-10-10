# Shared ESP32-S3 runtime services

Opt-in ESP-IDF component for NVS device configuration and bounded diagnostic logging. Board adapters choose display capability and optional diagnostic console sink. Keep controller initialization, radios, storage and pins outside this board-neutral component.

- Preserve the `aqcfg` NVS namespace, credential/PIN/token secrecy and explicit settings validation. One command worker writes settings; readers use the guarded snapshot.
- Every save writes the complete configuration as one versioned `cfg` blob with a CRC-32, one `nvs_set_blob` and one commit, then reads it back. NVS replaces a blob entry as a unit, and the CRC rejects a damaged record. A missing or invalid `cfg` falls back to the legacy per-key layout, which is read for migration and never erased or rewritten. A blob value passes the same validation as a legacy key. No hardware power-cut test proves this yet, so report persistence failures and do not claim measured power-cut durability.
- Configure the borrowed diagnostic callback sink before tasks start; it must outlive the log ring. Secret values are never added to CONFIG JSON or diagnostic logs.
- Configuration and log tests compile the real implementation against host SDK fakes. They establish logic and failure handling, not hardware NVS or power-loss durability.

Use the pinned native ESP-IDF environment. Before changing SDK/dependencies,
check current official documentation/latest stable versions and record exact
pins plus compatibility evidence. Never add Arduino Preferences, Print or APIs.

- Location normalizes after the complete patch using official H3; store only the published cell with its maximum resolution and owner-declared assigned ISO country. Increasing precision cannot upsample stored cells. Missing records use defaults; malformed stored location is unset and malformed NTP is disabled. Secrets remain absent from all configuration pages.
