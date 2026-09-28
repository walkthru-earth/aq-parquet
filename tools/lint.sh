#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"
options=(--language=c++ --enable=warning,style,performance,portability
  --inline-suppr --error-exitcode=1 --std=c++23 --max-configs=1
  '--suppress=*:*/vendor/lz4/*')
cppcheck "${options[@]}" \
  firmware/arduino-m5unified/bringup/bringup.ino \
  firmware/arduino-m5unified/bringup/telemetry_logger.cpp \
  firmware/common/connectivity/src/ble_sync.cpp \
  firmware/common/connectivity/src/wifi_link.cpp \
  firmware/common/connectivity/src/sync_service.cpp \
  firmware/common/connectivity/src/archive_sync.cpp \
  firmware/common/connectivity/src/control_sync.cpp \
  firmware/common/runtime/src/device_config.cpp \
  firmware/common/runtime/src/debug_log.cpp \
  firmware/common/src/parquet_writer.cpp \
  firmware/common/src/lz4_codec.cpp \
  firmware/common/src/pms_frame.cpp \
  firmware/common/logger/src/aq_logger.cpp \
  firmware/common/logger/src/aq_logger_status.cpp \
  firmware/common/logger/src/aq_logger_provision.cpp \
  firmware/arduino-waveshare-sim7670g/diagnostic/diagnostic.ino
# The diagnostic explicitly rejects PSRAM; the application explicitly needs it.
# Check each sketch in the configuration it is actually compiled with.
cppcheck "${options[@]}" -DBOARD_HAS_PSRAM=1 \
  firmware/arduino-waveshare-sim7670g/logger/logger.ino
