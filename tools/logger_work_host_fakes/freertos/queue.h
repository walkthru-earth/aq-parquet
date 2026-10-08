#pragma once
#include "FreeRTOS.h"
#include <functional>
struct FakeQueue {
  unsigned depth = 0;
  unsigned capacity = 8;
};
using QueueHandle_t = FakeQueue *;
inline std::function<void()> before_idle_wait;
inline int xQueueSend(QueueHandle_t queue, const void *, TickType_t) {
  if (queue->depth == queue->capacity)
    return 0;
  ++queue->depth;
  return pdTRUE;
}
inline unsigned uxQueueMessagesWaiting(QueueHandle_t queue) {
  const auto depth = queue->depth;
  if (!depth && before_idle_wait) {
    auto callback = before_idle_wait;
    before_idle_wait = {};
    callback();
  }
  return depth;
}
