#pragma once

#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

namespace aqlogger::work {
enum class StallAction : std::uint8_t { None, Warn, Restart };

// Copy first, then signal. The binary wakeup may coalesce several enqueues;
// the queues retain every item and the worker checks them before sleeping.
inline bool enqueue(QueueHandle_t queue, const void *item,
                    SemaphoreHandle_t wakeup) {
  if (xQueueSend(queue, item, 0) != pdTRUE)
    return false;
  xSemaphoreGive(wakeup);
  return true;
}

inline void wait_if_idle(QueueHandle_t samples, SemaphoreHandle_t wakeup) {
  if (uxQueueMessagesWaiting(samples) == 0)
    // An enqueue between the queue check and this take leaves a pending signal.
    // Periodic wakeup also keeps worker-heartbeat monitoring bounded.
    xSemaphoreTake(wakeup, pdMS_TO_TICKS(1000));
}

// Evaluates storage-worker heartbeat liveness on the main loop.
// - Returns None before the first heartbeat or within the 5 s normal window.
// - Returns Restart when restart_s > 0 and elapsed exceeds max(restart_s, 5) s.
// - Returns Warn when elapsed > 5 s and >= 30 s have passed since the previous
//   warning.
inline StallAction check_stall(std::int64_t now_us, std::int64_t heartbeat_us,
                               std::int64_t &last_warning_us,
                               std::uint32_t restart_s) {
  if (heartbeat_us <= 0 || now_us <= heartbeat_us)
    return StallAction::None;
  const std::int64_t elapsed_us = now_us - heartbeat_us;
  if (restart_s > 0) {
    const std::int64_t restart_us =
        static_cast<std::int64_t>(restart_s < 5U ? 5U : restart_s) * 1000000LL;
    if (elapsed_us > restart_us)
      return StallAction::Restart;
  }
  if (elapsed_us > 5000000LL &&
      (last_warning_us == 0 || now_us - last_warning_us > 30000000LL)) {
    last_warning_us = now_us;
    return StallAction::Warn;
  }
  return StallAction::None;
}
} // namespace aqlogger::work
