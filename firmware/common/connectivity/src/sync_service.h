#pragma once

#include "ble_sync.h"
#include "wifi_link.h"

#include <cstddef>

namespace ossync {
struct StartResult {
  bool settings_loaded = false;
  bool ble_started = false;
  bool lan_started = false;
};

// Sync service tuning with shared defaults. A board passes its own values
// through opensensor::logger::Config::sync; queue depths size the logger's
// queues.
struct ServiceConfig {
  ble::Options ble{};
  lan::Options lan{};
  std::size_t command_queue_depth = 6;
  std::size_t sample_queue_depth = 8;
};

// Call once after the board's storage/command queue is ready. Identity strings
// must remain valid for the firmware lifetime. The handler is called from both
// transport tasks and must enqueue/copy without blocking or touching hardware.
// Wi-Fi remains off until the stored configuration enables it.
StartResult begin(const ble::Identity &identity, ble::RequestHandler handler,
                  bool display_detected, const ServiceConfig &config = {});
} // namespace ossync
