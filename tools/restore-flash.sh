#!/usr/bin/env bash
set -euo pipefail

board=""
port=""
image=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --board) board="${2:-}"; shift 2 ;;
    --port) port="${2:-}"; shift 2 ;;
    --*) printf 'unknown option: %s\n' "$1" >&2; exit 2 ;;
    *)
      if [[ -n "${image}" ]]; then
        printf 'only one backup image may be restored\n' >&2
        exit 2
      fi
      image="$1"; shift ;;
  esac
done
if [[ -z "${port}" || -z "${image}" ]]; then
  printf 'usage: %s --board {m5stack-cores3|waveshare-sim7670g-v2} --port <checked-port> backup/<matching-file>.bin\n' "$0" >&2
  exit 2
fi
case "${board}:$(basename "${image}")" in
  m5stack-cores3:m5stack-cores3-flash-*.bin|m5stack-cores3:cores3-flash-*.bin|waveshare-sim7670g-v2:waveshare-sim7670g-v2-flash-*.bin) ;;
  *) printf 'backup name does not match board %s: %s\n' "${board}" "${image}" >&2; exit 2 ;;
esac
if [[ ! -f "${image}" || "$(stat -f %z "${image}")" != 16777216 ]]; then
  printf 'expected a complete 16777216-byte image: %s\n' "${image}" >&2
  exit 2
fi
# Keep the transport at esptool's default speed. The checked port and image
# remain explicit because this operation overwrites the entire flash.
exec python -m esptool --chip esp32s3 --port "${port}" write-flash 0 "${image}"
