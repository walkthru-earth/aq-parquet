#pragma once

#include "ble_sync.h"

namespace aqsync {
struct StartResult {
  bool settings_loaded = false;
  bool ble_started = false;
  bool lan_started = false;
};

// Call once after the board's storage/command queue is ready. Identity strings
// must remain valid for the firmware lifetime. The handler is called from both
// transport tasks and must enqueue/copy without blocking or touching hardware.
// Wi-Fi remains off until the stored configuration enables it.
StartResult begin(const ble::Identity &identity, ble::RequestHandler handler,
                  bool display_detected);
} // namespace aqsync
