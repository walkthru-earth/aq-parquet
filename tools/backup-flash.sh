#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
board=""
port=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --board) board="${2:-}"; shift 2 ;;
    --port) port="${2:-}"; shift 2 ;;
    *) printf 'unknown argument: %s\n' "$1" >&2; exit 2 ;;
  esac
done
case "${board}" in
  m5stack-cores3|waveshare-sim7670g-v2) ;;
  *) printf 'usage: %s --board {m5stack-cores3|waveshare-sim7670g-v2} --port <checked-port>\n' "$0" >&2; exit 2 ;;
esac
if [[ -z "${port}" ]]; then
  printf 'a checked --port is required\n' >&2
  exit 2
fi
# Only use this after flash-id has independently confirmed 16 MB on this port.
backup_dir="${repo_root}/backup"
image="${backup_dir}/${board}-flash-$(date -u +%Y%m%dT%H%M%SZ).bin"
mkdir -p "${backup_dir}"
# Keep esptool at its default serial speed. A failed read must not leave an
# apparently valid image; verify its exact capacity before reporting success.
python -m esptool --chip esp32s3 --port "${port}" read-flash 0 0x1000000 "${image}"
bytes="$(stat -f %z "${image}")"
if [[ "${bytes}" != 16777216 ]]; then
  printf 'backup size mismatch: %s bytes in %s\n' "${bytes}" "${image}" >&2
  exit 1
fi
/usr/bin/shasum -a 256 "${image}"
printf 'backup verified: %s (%s bytes)\n' "${image}" "${bytes}"
