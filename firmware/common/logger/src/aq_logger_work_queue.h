#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

namespace aqlogger::work {
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
} // namespace aqlogger::work
