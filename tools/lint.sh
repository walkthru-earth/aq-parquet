#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"
# The C++ standard below must match the firmware helper and the host tests.
python tools/check_idf_target.py --cxx-dialect
# Shared code takes board capabilities from hooks, ServiceConfig and sdkconfig
# defaults, never from a board name (AGENTS.md, code boundaries). Fail on an
# #if, #ifdef, #ifndef or #elif in firmware/common that names a board, a modem
# or a chip target anywhere in the condition, also inside a longer macro name
# such as CONFIG_AQ_BOARD_CORES3 and on a line continued with a backslash.
# Third-party vendor code is skipped. CONFIG_IDF_TARGET_ARCH_* names a CPU
# family, not a chip, and stays allowed.
board_names='CORES3|CORE_S3|M5STACK|M5_?CORE|WAVESHARE|SIM7[0-9]{3}|CONFIG_IDF_TARGET(\b|_ESP)'
board_pattern="(?im)^[ \\t]*#[ \\t]*(if|ifdef|ifndef|elif|elifdef|elifndef)\\b([^\\n\\\\]|\\\\.|\\\\\\n)*?(${board_names})"
if rg -U -n -g '*.{c,cc,cpp,h,hpp,inc}' -g '!**/vendor/**' "${board_pattern}" firmware/common; then
  printf '!!! board-name preprocessor conditional in firmware/common, use hooks or sdkconfig capabilities\n' >&2
  exit 1
fi
options=(--language=c++ --enable=warning,style,performance,portability
  --inline-suppr --error-exitcode=1 --std=c++23 --max-configs=1
  '--suppress=*:*/vendor/lz4/*')
cppcheck "${options[@]}" \
  firmware/esp-idf-cores3/bringup/main.cpp \
  firmware/esp-idf-cores3/bringup/telemetry_logger.cpp \
  firmware/common/connectivity/src/ble_sync.cpp \
  firmware/common/connectivity/src/ble_policy.cpp \
  firmware/common/connectivity/src/wifi_link.cpp \
  firmware/common/connectivity/src/network_time.cpp \
  firmware/common/location/src/aq_location.cpp \
  firmware/common/connectivity/src/sync_service.cpp \
  firmware/common/connectivity/src/archive_sync.cpp \
  firmware/common/connectivity/src/control_sync.cpp \
  firmware/common/runtime/src/device_config.cpp \
  firmware/common/runtime/src/debug_log.cpp \
  firmware/common/runtime/src/aq_console.cpp \
  firmware/common/runtime/src/coredump_report.cpp \
  firmware/esp-idf-waveshare-sim7670g/board_io.cpp \
  firmware/common/src/parquet_writer.cpp \
  firmware/common/src/parquet_reader.cpp \
  firmware/common/src/lz4_codec.cpp \
  firmware/common/sensors/src/pms_frame.cpp \
  firmware/common/power/src/aq_battery_health.cpp \
  firmware/common/logger/src/aq_logger.cpp \
  firmware/common/logger/src/aq_logger_status.cpp \
  firmware/common/logger/src/aq_logger_provision.cpp \
  firmware/common/logger/src/aq_logger_live.cpp \
  firmware/esp-idf-waveshare-sim7670g/diagnostic/main.cpp
# The diagnostic explicitly rejects PSRAM; the application explicitly needs it.
# Check each sketch in the configuration it is actually compiled with.
cppcheck "${options[@]}" -DCONFIG_SPIRAM=1 \
  firmware/esp-idf-waveshare-sim7670g/logger/main.cpp
