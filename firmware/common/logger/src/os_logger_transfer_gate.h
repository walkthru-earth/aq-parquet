#pragma once

#include "os_logger_emergency_close.h"

#include <ble_sync.h>
#include <cstddef>
#include <cstdint>

// How a due emergency close stops a BLE or LAN LIST or READ in flight. Only the
// storage worker calls these, between frames it is about to send. No IO and
// no clocks, so the host tests drive the same code as the firmware.
namespace oslogger::power {

// CHUNK and FILE frames carry transfer data. A terminal frame (READ_END,
// LIST_END, ERROR) is never refused or cut short, so a stopped transfer
// still ends with a frame the client understands.
inline bool data_frame(const std::uint8_t *frame, std::size_t length) {
  return frame && length &&
         (frame[0] == ble::kFrameChunk || frame[0] == ble::kFrameFile);
}

// Offered a frame. A READ CHUNK is refused once a close is due, so READ
// ends with READ_END busy. LIST is not refused here. The archive session's
// preempted hook reads the same close and ends the job before its next FILE
// with ERROR busy "preempted" (archive_sync.h).
inline bool refuses_frame(const EmergencyClose &close,
                          const std::uint8_t *frame, std::size_t length,
                          std::int64_t now_ms) {
  return frame && length && frame[0] == ble::kFrameChunk &&
         close.preempt(now_ms);
}

// Between ring space waits. Any frame stops waiting once an ABORT overtook
// it. A data frame also stops once a close falls due and ends like a
// refused frame, so a peer that stopped draining cannot hold the close back
// for the whole slow peer bound.
inline bool keep_waiting(const EmergencyClose &close, bool abort_pending,
                         bool data, std::int64_t now_ms) {
  return !abort_pending && !(data && close.preempt(now_ms));
}

} // namespace oslogger::power
