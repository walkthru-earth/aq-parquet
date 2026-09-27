#!/usr/bin/env bash
set -euo pipefail

# espefuse requires global options such as --port before the subcommand.
exec python -m espefuse "$@" summary
