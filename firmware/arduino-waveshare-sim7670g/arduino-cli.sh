#!/usr/bin/env bash
set -euo pipefail

trial_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
toolchain_root="${AQ_TOOLCHAIN_ROOT:-${M5_TOOLCHAIN_ROOT:-${HOME}/.cache/m5stack-aq-parquet/toolchains}}"

export ARDUINO_DIRECTORIES_DATA="${toolchain_root}/arduino/data"
export ARDUINO_DIRECTORIES_DOWNLOADS="${toolchain_root}/arduino/downloads"
export ARDUINO_DIRECTORIES_USER="${toolchain_root}/arduino/user"
export ARDUINO_BOARD_MANAGER_ADDITIONAL_URLS="https://espressif.github.io/arduino-esp32/package_esp32_index.json"

arduino_cli="${toolchain_root}/bin/arduino-cli"
if [[ ! -x "${arduino_cli}" ]]; then
  printf 'arduino-cli is not installed; run %s/setup.sh first\n' "${trial_dir}" >&2
  exit 1
fi

exec "${arduino_cli}" "$@"
