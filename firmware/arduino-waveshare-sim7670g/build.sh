#!/usr/bin/env bash
set -euo pipefail
trial_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=dependencies.lock
source "${trial_dir}/dependencies.lock"
build_dir="${trial_dir}/build"
mkdir -p "${build_dir}"
"${trial_dir}/arduino-cli.sh" compile \
  --fqbn "${WAVESHARE_FQBN}" \
  --libraries "${trial_dir}/../common" \
  --warnings all \
  --build-path "${build_dir}" \
  --export-binaries \
  "${trial_dir}/diagnostic"
grep -qx 'CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y' "${build_dir}/sdkconfig"
# The prebuilt Arduino SDK's sdkconfig has generic SPIRAM support even when
# PSRAM=disabled in the board FQBN. The sketch rejects BOARD_HAS_PSRAM.
