#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
board="${1:-}"
port="${2:-}"
backup="${3:-}"
case "${board}" in
  cores3) trial_dir="${repo_root}/firmware/esp-idf-cores3"; variant=bringup; backup_board=m5stack-cores3 ;;
  waveshare) trial_dir="${repo_root}/firmware/esp-idf-waveshare-sim7670g"; variant="${4:-logger}"; backup_board=waveshare-sim7670g-v2 ;;
  *) printf 'usage: %s {cores3|waveshare} <checked-port> <full-backup> [logger|diagnostic]\n' "$0" >&2; exit 2 ;;
esac
if [[ -z "${port}" || ! -c "${port}" || ! -f "${backup}" ]]; then
  printf 'checked serial device and full backup required\n' >&2; exit 2
fi
case "$(basename "${backup}")" in
  "${backup_board}"-flash-*.bin) ;;
  *) printf 'backup does not match selected board\n' >&2; exit 2 ;;
esac
if [[ "$(stat -f %z "${backup}")" != 16777216 ]]; then
  printf 'backup must be exactly 16777216 bytes\n' >&2; exit 2
fi
case "${board}/${variant}" in
  cores3/bringup|waveshare/logger|waveshare/diagnostic) ;;
  *) printf 'invalid board variant\n' >&2; exit 2 ;;
esac
build_dir="${trial_dir}/build/${variant}"
if [[ ! -f "${build_dir}/aq-build-target.txt" ]] || [[ "$(cat "${build_dir}/aq-build-target.txt")" != "${board}/${variant}" ]]; then
  printf 'build and verify the selected native firmware first\n' >&2; exit 2
fi
python "${repo_root}/tools/check_idf_target.py" "${build_dir}" --board "${board}" --variant "${variant}" --verify-manifest --backup "${backup}"
# flash the reviewed build without silently rebuilding it; never erase NVS.
# ESP-IDF 6.1 has no --no-deps option. Its normal flash action depends on a
# build, so consume the checked generated response file with pinned Pixi
# esptool directly. Only the manifest-verified four regions are written.
cd "${build_dir}"
exec python -m esptool --chip esp32s3 --port "${port}" --baud 115200 \
  --before default-reset --after hard-reset write-flash "@flash_args"
