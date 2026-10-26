#pragma once

// Board glue for the persistent DebugLog block. Call once from app_main right
// after opensensor::console::begin(). With
// CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY and PSRAM present at runtime
// it attaches a block in PSRAM .noinit, keeping the previous boot's lines and
// disconnect events across a warm reboot. Otherwise the internal ring stays
// in use and this returns false; the build never depends on PSRAM.
//
// Sizes are board configuration. CONFIG_OS_LOG_PERSIST_RING_BYTES and
// CONFIG_OS_LOG_PERSIST_EVENT_SLOTS (runtime Kconfig, set per board in
// sdkconfig.defaults) size the reserved block and form default_layout(). A
// board may pass a smaller layout. One larger than the reservation is
// refused and the internal ring stays.

#include "debug_log.h"

namespace opensensor::log {
DebugLog::Layout default_layout();
bool attach_persistent(const DebugLog::Layout &layout = default_layout());
// Call once after nvs_flash_init() succeeds. With a core dump saved in flash
// that no earlier boot reported, writes one COREDUMP line and records the
// dump's size and SHA-256 in NVS namespace "osdump". The dump stays in flash
// for idf.py coredump-info. A no-op unless CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH.
void report_coredump();
} // namespace opensensor::log
