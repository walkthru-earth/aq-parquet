#!/usr/bin/env bash
# Host gate shared by local runs and CI. Run it as `pixi run gate`.
#
# Every task below is an existing pixi task, run with its sanitizer flag when
# the script supports one. Tasks that need a board, a serial port, Bluetooth,
# a LAN peer, the network or the ESP-IDF SDK are deliberately absent. The SDK
# builds run in the CI board job instead.
#
# Each task runs under GNU timeout from the pixi coreutils pin. A task that
# outlives AQ_GATE_TASK_TIMEOUT seconds (default 300) is stopped, reported as
# TIMEOUT by name, and killed 10 s later if it ignores SIGTERM. Anything left
# in the task's process group is then killed, so no stray process outlives it.
#
# Usage: tools/gate.sh [--keep-going]
#   --keep-going  run every task and report all failures at the end
set -uo pipefail

keep_going=0
case "${1:-}" in
  "") ;;
  --keep-going) keep_going=1 ;;
  *) printf 'usage: %s [--keep-going]\n' "$0" >&2; exit 2 ;;
esac

tasks=(
  # Formatting and static diagnostics
  "fmt-check"
  "lint"
  # Writer, reader, schema and dictionary contracts
  "common-test"
  "parquet-test --sanitize"
  "reader-test"
  "size-survey-test"
  "telemetry-contract-test --sanitize"
  "waveshare-contract-test --sanitize"
  # Sensor drivers on fake buses
  "pms-frame-test"
  "ltr553-test"
  "max17048-test"
  # Archive, transport and radio policy fixtures
  "archive-sync-test"
  "control-sync-test"
  "disconnect-test"
  "frame-ring-test"
  "ble-policy-test"
  "ble-advert-test"
  "ble-sync-test"
  "wifi-link-test"
  # Settings, time and location
  "config-test"
  "network-time-test"
  "location-test"
  # Logger lifecycle, storage and persistence
  "debug-log-test"
  "logger-work-queue-test"
  "logger-watchdog-test"
  "logger-status-test"
  "logger-rotation-test"
  "logger-directory-test"
  "logger-archive-reset-test"
  "logger-provision-test"
  "logger-live-test"
  "storage-governor-test"
  "crc-cache-test"
  "sd-transfer-buffer-test"
  # Offline backup publication with a fake esptool
  "backup-test"
)

task_timeout=${AQ_GATE_TASK_TIMEOUT:-300}
group=""
# The task group is not the terminal's foreground group, so Ctrl-C and a CI
# cancel reach only this script. Pass them on to the running task.
stop() {
  if [[ -n "${group}" ]]; then
    kill -TERM -- "-${group}" 2>/dev/null || true
  fi
  printf '\n!!! gate interrupted\n' >&2
  exit 130
}
trap stop INT TERM
passed=()
failed=()
started=${SECONDS}
for entry in "${tasks[@]}"; do
  read -r -a task <<<"${entry}"
  printf '\n==> pixi run %s\n' "${entry}"
  task_start=${SECONDS}
  # GNU timeout leads its own process group, so its PID is the group ID.
  timeout --kill-after=10 "${task_timeout}" pixi run "${task[@]}" &
  group=$!
  wait "${group}"
  status=$?
  # timeout exits as soon as pixi does. A grandchild forked while SIGTERM was
  # in flight would outlive it, so sweep the whole group before moving on.
  kill -KILL -- "-${group}" 2>/dev/null || true
  if [[ ${status} -eq 0 ]]; then
    passed+=("${entry} ($((SECONDS - task_start))s)")
  else
    # GNU timeout exits 124 after SIGTERM and 137 after the follow-up SIGKILL.
    if [[ ${status} -eq 124 || ${status} -eq 137 ]]; then
      failed+=("${entry} (TIMEOUT after ${task_timeout}s)")
    else
      failed+=("${entry} (exit ${status})")
    fi
    printf '\n!!! FAILED: pixi run %s, %s\n' "${entry}" "${failed[${#failed[@]}-1]}" >&2
    if [[ ${keep_going} -eq 0 ]]; then
      break
    fi
  fi
done

printf '\n==> gate summary, %d passed, %d failed, %ds\n' \
  "${#passed[@]}" "${#failed[@]}" "$((SECONDS - started))"
for entry in ${passed[@]+"${passed[@]}"}; do printf '  ok    %s\n' "${entry}"; done
for entry in ${failed[@]+"${failed[@]}"}; do printf '  FAIL  %s\n' "${entry}"; done
if [[ ${#failed[@]} -gt 0 ]]; then
  exit 1
fi
