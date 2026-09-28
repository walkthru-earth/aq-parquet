#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
trial_dir="${repo_root}/firmware/arduino-m5unified"
source "${trial_dir}/dependencies.lock"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/aq-logger-compile.XXXXXX")"
trap 'rm -rf "${build_dir}"' EXIT
"${trial_dir}/arduino-cli.sh" compile \
  --fqbn 'esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,USBMode=hwcdc,CDCOnBoot=default' \
  --library "${repo_root}/firmware/common" \
  --library "${repo_root}/firmware/common/runtime" \
  --library "${repo_root}/firmware/common/connectivity" \
  --library "${repo_root}/firmware/common/logger" \
  --warnings all --build-path "${build_dir}" \
  "${repo_root}/firmware/common/logger/tests/compile"
if [[ -d "${build_dir}/libraries/M5Unified" || -d "${build_dir}/libraries/M5GFX" ]]; then
  printf 'logger fixture unexpectedly depends on M5 libraries\n' >&2
  exit 1
fi
