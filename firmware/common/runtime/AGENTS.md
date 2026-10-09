# Shared ESP32-S3 runtime services

Opt-in ESP-IDF component for NVS device configuration and bounded diagnostic logging. Board adapters choose display capability and optional diagnostic console sink. Keep controller initialization, radios, storage and pins outside this board-neutral component.

- Preserve the `aqcfg` NVS namespace, credential/PIN/token secrecy and explicit settings validation. One command worker writes settings; readers use the guarded snapshot.
- NVS settings currently commit per key, not as one transactional record. Report persistence failures and do not claim whole-record power-cut atomicity.
- Configure the borrowed diagnostic callback sink before tasks start; it must outlive the log ring. Secret values are never added to CONFIG JSON or diagnostic logs.
- Configuration and log tests compile the real implementation against host SDK fakes. They establish logic and failure handling, not hardware NVS or power-loss durability.

Use the pinned native ESP-IDF environment. Before changing SDK/dependencies,
check current official documentation/latest stable versions and record exact
pins plus compatibility evidence. Never add Arduino Preferences, Print or APIs.
