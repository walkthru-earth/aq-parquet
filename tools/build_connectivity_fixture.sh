#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Use the same exact Arduino/NimBLE versions verified by the existing consumer,
# but compile for generic ESP32-S3 with no M5 board, library or trial source.
trial_dir="${repo_root}/firmware/arduino-m5unified"
source "${trial_dir}/dependencies.lock"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/aq-connectivity-compile.XXXXXX")"
trap 'rm -rf "${build_dir}"' EXIT
"${trial_dir}/arduino-cli.sh" compile \
  --fqbn 'esp32:esp32:esp32s3:FlashSize=16M,PSRAM=disabled,USBMode=hwcdc,CDCOnBoot=default' \
  --library "${repo_root}/firmware/common" \
  --library "${repo_root}/firmware/common/runtime" \
  --library "${repo_root}/firmware/common/connectivity" \
  --warnings all --build-path "${build_dir}" \
  "${repo_root}/tools/fixtures/connectivity_compile"
# Assert the fixture didn't acquire M5 board dependencies through a shared API.
if [[ -d "${build_dir}/libraries/M5Unified" || -d "${build_dir}/libraries/M5GFX" ]]; then
  printf 'compile fixture unexpectedly depends on M5 libraries\n' >&2
  exit 1
fi
