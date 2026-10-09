#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${repo_root}/tools/idf-dependencies.lock"
toolchain_root="${AQ_TOOLCHAIN_ROOT:-${M5_TOOLCHAIN_ROOT:-${HOME}/.cache/m5stack-aq-parquet/toolchains}}"
idf_path="${AQ_IDF_PATH:-${toolchain_root}/esp-idf-v${ESP_IDF_VERSION}}"
export IDF_TOOLS_PATH="${AQ_IDF_TOOLS_PATH:-${HOME}/.espressif}"
if [[ ! -d "${idf_path}" ]]; then
  mkdir -p "$(dirname "${idf_path}")"
  git clone --depth 1 --branch "v${ESP_IDF_VERSION}" https://github.com/espressif/esp-idf.git "${idf_path}"
fi
if [[ "$(git -C "${idf_path}" rev-parse HEAD)" != "${ESP_IDF_COMMIT}" ]]; then
  printf 'Refusing to change a different SDK checkout: %s\n' "${idf_path}" >&2
  exit 2
fi
python "${repo_root}/tools/check_idf_target.py" --sdk-only "${idf_path}" --allow-missing-submodules
# Fetch the SDK's exact submodule commits; do not update to floating branches.
git -C "${idf_path}" submodule update --init --recursive --depth 1
python "${repo_root}/tools/check_idf_target.py" --sdk-only "${idf_path}"
export IDF_PATH="${idf_path}"
"${idf_path}/install.sh" esp32s3
printf 'ESP-IDF v%s ready at %s\n' "${ESP_IDF_VERSION}" "${idf_path}"
