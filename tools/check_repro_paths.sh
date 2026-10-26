#!/usr/bin/env bash
# Build one board image from two checkout paths and compare the image hashes.
#
# Both checkouts are detached git worktrees of HEAD, so uncommitted edits are
# not part of the check. Their paths differ in length, so an absolute path
# that leaks into the image shifts every later byte. SOURCE_DATE_EPOCH is the
# HEAD commit time for both builds, which pins the __DATE__ and __TIME__ the
# SDK writes into the app description without changing sdkconfig. Release
# builds keep the real build time.
#
# tools/repro_paths.py compares the two os-build-sha256.json manifests. The
# compiler database and project description hold absolute paths by design,
# every other file, the flashed images included, must match byte for byte.
#
# Usage: tools/check_repro_paths.sh [cores3|waveshare] [logger|diagnostic]
#   Defaults to waveshare logger, which builds every common component.
#   OS_REPRO_KEEP=1 keeps both worktrees for inspection.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
board="${1:-waveshare}"
case "${board}" in
  cores3) trial=esp-idf-cores3; variant=bringup ;;
  waveshare) trial=esp-idf-waveshare-sim7670g; variant="${2:-logger}" ;;
  *) printf 'usage: %s [cores3|waveshare] [logger|diagnostic]\n' "$0" >&2; exit 2 ;;
esac

if [[ -n "$(git -C "${repo_root}" status --porcelain --untracked-files=no)" ]]; then
  printf 'note: uncommitted changes are not built, the check uses HEAD\n' >&2
fi
work="$(mktemp -d "${TMPDIR:-/tmp}/os-repro.XXXXXX")"
first="${work}/a/opensensor-space-firmware"
second="${work}/second-checkout-path/opensensor-space-firmware-repro"
cleanup() {
  if [[ "${OS_REPRO_KEEP:-0}" == 1 ]]; then
    printf 'kept %s and %s\n' "${first}" "${second}" >&2
    return
  fi
  git -C "${repo_root}" worktree remove --force "${first}" >/dev/null 2>&1 || true
  git -C "${repo_root}" worktree remove --force "${second}" >/dev/null 2>&1 || true
  rm -rf "${work}"
}
trap cleanup EXIT

SOURCE_DATE_EPOCH="$(git -C "${repo_root}" log -1 --format=%ct HEAD)"
export SOURCE_DATE_EPOCH
for checkout in "${first}" "${second}"; do
  mkdir -p "$(dirname "${checkout}")"
  git -C "${repo_root}" worktree add --detach --quiet "${checkout}" HEAD
  printf '== building %s/%s in %s\n' "${board}" "${variant}" "${checkout}"
  "${checkout}/tools/build-idf-board.sh" "${board}" "${variant}"
done

python "${repo_root}/tools/repro_paths.py" \
  "${first}" "${first}/firmware/${trial}/build/${variant}" \
  "${second}" "${second}/firmware/${trial}/build/${variant}"
