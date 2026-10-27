# Shared uploader (#81)

- Software only. Off by default through `CONFIG_OS_UPLOAD_ENABLE`. No task,
  no provisioning and no call to source.coop happens from this component
  until a later integration PR wires a `Session` into the storage worker.
- Never put a real or realistic `sck_` key, PSK, PIN or token in code,
  tests, logs or docs. Use `<sck key>` style placeholders and the public AWS
  example keys. The API key is only ever sent in the STS POST body, never in
  a URL, header or log line.
- Pure modules (`sigv4`, `sts`, `policy`, `ledger`, `session`) have no IO,
  heap, clocks or board names. `os_upload_device.*` is the only ESP-IDF
  code, a PSA SHA-256 and an esp_http_client transport with the cert bundle.
- Policy rules are load bearing, each one has a host test and a mutation
  proof. Finalized dated files only, oldest first, one per step. No upload
  until UTC comes from Host or Network, never from the RTC or a Wi-Fi link
  alone. Back off on transient errors. Stop on 401, 403 and rejected
  requests until the key generation changes. A dev target never writes
  outside `dev/opensensor-space-firmware/<board>/`.
- The ledger is append only. Never rewrite, truncate or delete it, and never
  delete, rename or relabel an archive file because of an upload. The
  object key is the prefix plus the SD relative name verbatim.
- The dev prefix lives in one constant, `kDevPrefix` in `os_upload_policy.h`.
- Host gate is `pixi run upload-test` with ASan and UBSan. The SigV4 vectors
  come from aws-c-auth under Apache-2.0, keep their attribution.
