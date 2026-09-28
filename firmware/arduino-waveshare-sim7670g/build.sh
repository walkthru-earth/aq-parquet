#!/usr/bin/env bash
set -euo pipefail
trial_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=dependencies.lock
source "${trial_dir}/dependencies.lock"
variant="${1:-logger}"
common_dir="${trial_dir}/../common"
case "${variant}" in
  logger) fqbn="${WAVESHARE_FQBN}" ;;
  diagnostic) fqbn="${WAVESHARE_DIAGNOSTIC_FQBN}" ;;
  *) printf 'usage: %s [logger|diagnostic]\n' "$0" >&2; exit 2 ;;
esac
build_dir="${trial_dir}/build/${variant}"
mkdir -p "${build_dir}"
libraries=(--library "${common_dir}")
if [[ "${variant}" == logger ]]; then
  libraries+=(--library "${common_dir}/runtime" --library "${common_dir}/connectivity" --library "${common_dir}/logger")
  dictionary_digest="$(sed -n 's/.*"\([0-9a-f]\{64\}\)".*/\1/p' "${trial_dir}/logger/telemetry_dictionary_digest.h")"
  printf '%s  %s\n' "${dictionary_digest}" "${trial_dir}/logger/telemetry_fields.inc" | /usr/bin/shasum -a 256 -c -
fi
"${trial_dir}/arduino-cli.sh" compile \
  --fqbn "${fqbn}" \
  "${libraries[@]}" \
  --warnings all \
  --build-path "${build_dir}" \
  --export-binaries \
  "${trial_dir}/${variant}"
rg -q '^CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y$' "${build_dir}/sdkconfig"
if [[ "${variant}" == logger ]]; then
  python "${trial_dir}/../../tools/check_arduino_target.py" "${build_dir}" "${fqbn}" --memory qio_opi
fi
printf '%s\n' "${fqbn}" > "${build_dir}/aq-build-target.txt"
