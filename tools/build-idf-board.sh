#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
board="${1:-}"
case "${board}" in
  cores3) trial_dir="${repo_root}/firmware/esp-idf-cores3"; variant=bringup ;;
  waveshare) trial_dir="${repo_root}/firmware/esp-idf-waveshare-sim7670g"; variant="${2:-logger}" ;;
  *) printf 'usage: %s {cores3|waveshare} [logger|diagnostic]\n' "$0" >&2; exit 2 ;;
esac
case "${board}/${variant}" in
  cores3/bringup|waveshare/logger|waveshare/diagnostic) ;;
  *) printf 'invalid board variant: %s/%s\n' "${board}" "${variant}" >&2; exit 2 ;;
esac
source "${trial_dir}/dependencies.env"
printf '%s  %s\n' \
  "${LZ4_C_SHA256}" "${repo_root}/firmware/common/vendor/lz4/lz4.c" \
  "${LZ4_H_SHA256}" "${repo_root}/firmware/common/vendor/lz4/lz4.h" \
  "${LZ4_LICENSE_SHA256}" "${repo_root}/firmware/common/vendor/lz4/LICENSE" | shasum -a 256 -c -
if [[ "${variant}" != diagnostic ]]; then
  digest="$(sed -n 's/.*"\([0-9a-f]\{64\}\)".*/\1/p' "${trial_dir}/${variant}/telemetry_dictionary_digest.h")"
  test "${#digest}" -eq 64
  printf '%s  %s\n' "${digest}" "${trial_dir}/${variant}/telemetry_fields.inc" | shasum -a 256 -c -
  # desc, the SHA-256 of the embedded board template (#85).
  python "${repo_root}/tools/check_board_template.py" "${trial_dir}/${variant}"
fi
# AQ_BENCH=stall builds a bench image with tools/idf-bench-stall-sdkconfig.defaults
# in its own directory, so its sdkconfig never leaks into the shipped build.
build_dir="${trial_dir}/build/${variant}"
target="${board}/${variant}"
idf_args=(-D "AQ_VARIANT=${variant}")
case "${AQ_BENCH:-}" in
  "") ;;
  stall)
    if [[ "${variant}" == diagnostic ]]; then
      printf 'AQ_BENCH=stall needs the logger runtime, not the diagnostic variant\n' >&2; exit 2
    fi
    build_dir="${build_dir}-bench-stall"
    target="${target} bench=stall"
    idf_args+=(-D "AQ_SDKCONFIG_OVERLAY=${repo_root}/tools/idf-bench-stall-sdkconfig.defaults") ;;
  *) printf 'unknown AQ_BENCH: %s (expected stall)\n' "${AQ_BENCH}" >&2; exit 2 ;;
esac
rm -f "${build_dir}/aq-build-target.txt" "${build_dir}/aq-build-sha256.json"
"${repo_root}/tools/idf.sh" -C "${trial_dir}" -B "${build_dir}" "${idf_args[@]}" build
if [[ "${variant}" != diagnostic ]]; then
  python "${repo_root}/tools/check_board_template.py" "${trial_dir}/${variant}" --build "${build_dir}"
fi
python "${repo_root}/tools/check_idf_target.py" "${build_dir}" --board "${board}" --variant "${variant}" --write-manifest
printf '%s\n' "${target}" > "${build_dir}/aq-build-target.txt"
