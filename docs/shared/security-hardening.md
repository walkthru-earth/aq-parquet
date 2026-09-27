# Firmware security and deployment hardening

This is an explicit boundary document for the CoreS3 Arduino logger (through v6.3). The Waveshare UART diagnostic does not provide BLE, LAN or file sync. It separates implemented controls from production work that has **not** been implemented or verified.

## Implemented

- BLE uses LE Secure Connections and bonding. Display-equipped CoreS3 devices show a fresh random passkey per pairing attempt. Display-less devices use a random per-device six-digit fixed PIN stored in NVS; operators can replace it with `ble.pin`, and legacy installations that still contain the old universal `123456` value rotate it at boot.
- BLE configuration does not return the fixed PIN or Wi-Fi PSK. LAN bearer tokens are 32 random bytes, returned only over an authenticated BLE link, can be rotated, and are never included in CONFIG JSON or normal logs.
- Wi-Fi and LAN are off unless provisioned/enabled. LAN token comparison is constant-time and a rejected challenger does not evict the authenticated session.
- Finalized files are structurally checked and immutable. Interrupted `.partial` files are moved to `/output/quarantine/` at startup, never silently repaired, published, or deleted. Bounded status counters expose total quarantine files and bytes across boots without exposing filenames.

## Development versus production

The current image is a development/field-trial firmware, not a hardened production image.

- LAN sync is authenticated but plaintext TCP. A bearer token does not provide confidentiality, server identity, forward secrecy, or protection against a LAN sniffer. Use it only on a trusted WPA2/WPA3 network, disable `lan.on` when unnecessary, rotate the token after suspected exposure, and use BLE for provisioning. TLS is not claimed: certificate/trust provisioning and ESP32-S3 memory, latency, and coexistence effects must be designed and measured first.
- Wi-Fi PSKs, BLE PINs, and LAN tokens are stored in NVS. This board currently has no verified flash encryption, so physical flash access can expose them. Serial logs may show a random pairing passkey while pairing is active; production deployments must restrict physical/serial access.
- `ble.pair=none` is encrypted Just Works without authenticated MITM protection and is for controlled development benches only.
- Six-digit BLE passkeys have limited entropy. Per-device generation removes a universal credential but does not make an unattended, physically exposed device resistant to determined nearby guessing or physical extraction.

## Not implemented or verified

This repository does **not** claim secure boot, flash encryption, signed OTA, anti-rollback, protected key storage, remote attestation, sensor calibration, SD-card encryption, or power-loss endurance. The partition table has OTA slots, but an authenticated/signed update flow is not implemented. Do not burn eFuses as part of normal development; irreversible provisioning needs a separate production design, recovery plan, and hardware verification.

Before production, define a manufacturing/provisioning ceremony, unique device ownership transfer and reset behavior, debug-port policy, TLS trust lifecycle, update signing/rollback, secret rotation/revocation, and destructive power-cut/full-card/removal tests on the exact board and card SKUs.
