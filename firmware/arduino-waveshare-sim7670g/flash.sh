#!/usr/bin/env bash
set -euo pipefail
trial_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=dependencies.lock
source "${trial_dir}/dependencies.lock"
port="${1:-}"
backup="${2:-}"
variant="${3:-logger}"
if [[ -z "${port}" || -z "${backup}" ]]; then
  printf 'usage: %s <checked-port> <verified-backup/waveshare-sim7670g-v2-flash-....bin> [logger|diagnostic]\n' "$0" >&2
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
case "${variant}" in
  logger) fqbn="${WAVESHARE_FQBN}" ;;
  diagnostic) fqbn="${WAVESHARE_DIAGNOSTIC_FQBN}" ;;
  *) printf 'unknown firmware variant: %s\n' "${variant}" >&2; exit 2 ;;
esac
build_dir="${trial_dir}/build/${variant}"
if [[ ! -f "${build_dir}/${variant}.ino.bin" || ! -f "${build_dir}/aq-build-target.txt" ]] ||
   [[ "$(cat "${build_dir}/aq-build-target.txt" 2>/dev/null)" != "${fqbn}" ]]; then
  printf 'build the selected firmware first: pixi run waveshare-build %s\n' "${variant}" >&2
  exit 2
fi
"${trial_dir}/arduino-cli.sh" upload \
  --fqbn "${fqbn}" \
  --port "${port}" \
  --input-dir "${build_dir}"
