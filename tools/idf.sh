#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${repo_root}/tools/idf-dependencies.lock"
toolchain_root="${AQ_TOOLCHAIN_ROOT:-${M5_TOOLCHAIN_ROOT:-${HOME}/.cache/m5stack-aq-parquet/toolchains}}"
export IDF_PATH="${AQ_IDF_PATH:-${toolchain_root}/esp-idf-v${ESP_IDF_VERSION}}"
export IDF_TOOLS_PATH="${AQ_IDF_TOOLS_PATH:-${HOME}/.espressif}"
if [[ ! -f "${IDF_PATH}/tools/idf.py" ]] || [[ "$(git -C "${IDF_PATH}" rev-parse HEAD)" != "${ESP_IDF_COMMIT}" ]]; then
  printf 'Pinned ESP-IDF v%s missing. Run pixi run idf-setup.\n' "${ESP_IDF_VERSION}" >&2
  exit 2
fi
python "${repo_root}/tools/check_idf_target.py" --sdk-only "${IDF_PATH}"
# SDK Python packages and compiler tools come from the pinned SDK installer.
# Host validators continue to run in Pixi, outside this subprocess environment.
export IDF_PYTHON_CHECK_CONSTRAINTS=yes
source "${IDF_PATH}/export.sh" >/dev/null
exec python "${IDF_PATH}/tools/idf.py" "$@"
