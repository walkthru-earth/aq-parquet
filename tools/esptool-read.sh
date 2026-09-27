#!/usr/bin/env bash
set -euo pipefail

command_name="${1:-}"
if [[ -z "${command_name}" ]]; then
  printf 'usage: %s {chip-id|flash-id|read-mac} [esptool global options]\n' "$0" >&2
  exit 2
fi
shift

# esptool 5 requires global options such as --port before the subcommand.
exec python -m esptool --chip esp32s3 "$@" "${command_name}"
