#!/usr/bin/env bash
set -euo pipefail

trial_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=dependencies.lock
source "${trial_dir}/dependencies.lock"

toolchain_root="${AQ_TOOLCHAIN_ROOT:-${M5_TOOLCHAIN_ROOT:-${HOME}/.cache/m5stack-aq-parquet/toolchains}}"
bin_dir="${toolchain_root}/bin"
arduino_cli="${bin_dir}/arduino-cli"

mkdir -p "${bin_dir}" "${toolchain_root}/arduino/data" \
  "${toolchain_root}/arduino/downloads" "${toolchain_root}/arduino/user"

installed_version=""
if [[ -x "${arduino_cli}" ]]; then
  installed_version="$("${arduino_cli}" version --format json | sed -n 's/.*"VersionString": "\([^"]*\)".*/\1/p')"
fi

if [[ "${installed_version}" != "${ARDUINO_CLI_VERSION}" ]]; then
  archive="arduino-cli_${ARDUINO_CLI_VERSION}_macOS_ARM64.tar.gz"
  url="https://github.com/arduino/arduino-cli/releases/download/v${ARDUINO_CLI_VERSION}/${archive}"
  temp_dir="$(mktemp -d)"
  trap 'rm -rf "${temp_dir}"' EXIT

  curl --fail --location --silent --show-error "${url}" --output "${temp_dir}/${archive}"
  printf '%s  %s\n' "${ARDUINO_CLI_MACOS_ARM64_SHA256}" "${temp_dir}/${archive}" | /usr/bin/shasum -a 256 --check
  tar -xzf "${temp_dir}/${archive}" -C "${temp_dir}" arduino-cli
  install -m 0755 "${temp_dir}/arduino-cli" "${arduino_cli}"
fi

"${trial_dir}/arduino-cli.sh" core update-index
"${trial_dir}/arduino-cli.sh" core install "esp32:esp32@${ARDUINO_ESP32_VERSION}"
"${trial_dir}/arduino-cli.sh" lib install "M5GFX@${M5GFX_VERSION}"
"${trial_dir}/arduino-cli.sh" lib install "M5Unified@${M5UNIFIED_VERSION}"
"${trial_dir}/arduino-cli.sh" lib install "NimBLE-Arduino@${NIMBLE_ARDUINO_VERSION}"

"${trial_dir}/arduino-cli.sh" version
"${trial_dir}/arduino-cli.sh" core list
"${trial_dir}/arduino-cli.sh" lib list | sed -n '/^M5GFX[[:space:]]/p;/^M5Unified[[:space:]]/p;/^NimBLE-Arduino[[:space:]]/p'
