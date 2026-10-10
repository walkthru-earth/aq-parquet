#pragma once

// Board glue for the persistent DebugLog block. Call once from app_main right
// after aq::console::begin(). With
// CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY and PSRAM present at runtime
// it attaches a block in PSRAM .noinit, keeping the previous boot's lines and
// disconnect events across a warm reboot. Otherwise the internal ring stays
// in use and this returns false; the build never depends on PSRAM.
//
// Sizes are board configuration. CONFIG_AQ_LOG_PERSIST_RING_BYTES and
// CONFIG_AQ_LOG_PERSIST_EVENT_SLOTS (runtime Kconfig, set per board in
// sdkconfig.defaults) size the reserved block and form default_layout(). A
// board may pass a smaller layout. One larger than the reservation is
// refused and the internal ring stays.

#include "debug_log.h"

namespace aq::log {
DebugLog::Layout default_layout();
bool attach_persistent(const DebugLog::Layout &layout = default_layout());
} // namespace aq::log
