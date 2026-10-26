#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${repo_root}/tools/idf-dependencies.lock"
# Sets toolchain_root, idf_path, IDF_TOOLS_PATH and IDF_PYTHON_ENV_PATH.
source "${repo_root}/tools/idf-env.sh"
export IDF_PATH="${idf_path}"
if [[ ! -f "${IDF_PATH}/tools/idf.py" ]] || [[ "$(git -C "${IDF_PATH}" rev-parse HEAD)" != "${ESP_IDF_COMMIT}" ]]; then
  printf 'Pinned ESP-IDF v%s missing. Run pixi run idf-setup.\n' "${ESP_IDF_VERSION}" >&2
  exit 2
fi
python "${repo_root}/tools/check_idf_target.py" --sdk-only "${IDF_PATH}"
# SDK Python packages and compiler tools come from the pinned SDK installer.
# Host validators continue to run in Pixi, outside this subprocess environment.
export IDF_PYTHON_CHECK_CONSTRAINTS=yes
# A new worktree, or a moved checkout, gets its venv here with no manual step.
ensure_idf_python_env
source "${IDF_PATH}/export.sh" >/dev/null
exec python "${IDF_PATH}/tools/idf.py" "$@"
