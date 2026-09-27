#pragma once

#include "ble_sync.h"

namespace aqsync {
// Synchronous worker-side replies; may wrap the same BLE/LAN response adapters
// used by ArchiveSession. A reply can block on transport back-pressure, so this
// service must never execute on a radio callback task.
struct ControlReplies {
  void *context = nullptr;
  std::uint16_t (*payload_max)(void *, ble::Link) = nullptr;
  bool (*respond)(void *, ble::Link, const std::uint8_t *,
                  std::size_t) = nullptr;
  bool (*error)(void *, ble::Link, ble::Op, ble::Error, const char *) = nullptr;
};

// Executes GET/SET_CONFIG, LOG_TAIL, GET_TOKEN and busy WIFI_SCAN replies.
// Clock, STATUS, flush and reboot remain runtime operations. One command worker
// owns this service and config writes; returns false for a runtime operation.
bool handle_common_control(const ble::ControlRequest &request,
                           const ControlReplies &replies);
} // namespace aqsync
