#!/usr/bin/env bash
set -euo pipefail
trial_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=dependencies.lock
source "${trial_dir}/dependencies.lock"
port="${1:-}"
backup="${2:-}"
if [[ -z "${port}" || -z "${backup}" ]]; then
  printf 'usage: %s <checked-port> <verified-backup/waveshare-sim7670g-v2-flash-....bin>\n' "$0" >&2
  exit 2
fi
case "$(basename "${backup}")" in
  waveshare-sim7670g-v2-flash-*.bin) ;;
  *) printf 'expected a Waveshare V2 full-flash backup, got: %s\n' "${backup}" >&2; exit 2 ;;
esac
if [[ ! -f "${backup}" || "$(stat -f %z "${backup}")" != 16777216 ]]; then
  printf 'verified 16777216-byte backup required before flashing: %s\n' "${backup}" >&2
  exit 2
fi
if [[ ! -f "${trial_dir}/build/diagnostic.ino.bin" ]]; then
  printf 'build the diagnostic first: pixi run waveshare-build\n' >&2
  exit 2
fi
"${trial_dir}/arduino-cli.sh" upload \
  --fqbn "${WAVESHARE_FQBN}" \
  --port "${port}" \
  --input-dir "${trial_dir}/build"
