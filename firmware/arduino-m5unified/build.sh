#!/usr/bin/env bash
set -euo pipefail

trial_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
common_dir="${trial_dir}/../common"
build_dir="${trial_dir}/build"
# shellcheck source=dependencies.lock
source "${trial_dir}/dependencies.lock"

# Verify the committed codec implementation, not a floating downloaded library.
printf '%s  %s\n' \
  "${LZ4_C_SHA256}" "${common_dir}/vendor/lz4/lz4.c" \
  "${LZ4_H_SHA256}" "${common_dir}/vendor/lz4/lz4.h" \
  "${LZ4_LICENSE_SHA256}" "${common_dir}/vendor/lz4/LICENSE" | /usr/bin/shasum -a 256 -c -

mkdir -p "${build_dir}"
# Dictionary digest is compiled into every file. Refuse stale provenance.
dictionary_digest=$(sed -n 's/^    "\([0-9a-f]\{64\}\)";/\1/p' \
  "${trial_dir}/bringup/telemetry_dictionary_digest.h")
test "${#dictionary_digest}" -eq 64
printf '%s  %s\n' "${dictionary_digest}" \
  "${trial_dir}/bringup/telemetry_fields.inc" | /usr/bin/shasum -a 256 -c -
"${trial_dir}/arduino-cli.sh" compile \
  --fqbn "${CORES3_FQBN}" \
  --library "${common_dir}" \
  --library "${common_dir}/runtime" \
  --library "${common_dir}/connectivity" \
  --warnings all \
  --build-path "${build_dir}" \
  --export-binaries \
  "${trial_dir}/bringup"

grep -qx 'CONFIG_SPIRAM_MODE_QUAD=y' "${build_dir}/sdkconfig"
grep -qx '# CONFIG_SPIRAM_MODE_OCT is not set' "${build_dir}/sdkconfig"
grep -qx 'CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y' "${build_dir}/sdkconfig"
grep -qx 'CONFIG_PARTITION_TABLE_CUSTOM=y' "${build_dir}/sdkconfig"
