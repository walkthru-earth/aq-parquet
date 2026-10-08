#pragma once
#include "FreeRTOS.h"
struct FakeSemaphore {
  bool pending = false;
  unsigned waits = 0;
  unsigned blocked = 0;
};
using SemaphoreHandle_t = FakeSemaphore *;
inline int xSemaphoreGive(SemaphoreHandle_t wakeup) {
  wakeup->pending = true;
  return pdTRUE;
}
inline int xSemaphoreTake(SemaphoreHandle_t wakeup, TickType_t timeout) {
  ++wakeup->waits;
  if (wakeup->pending) {
    wakeup->pending = false;
    return pdTRUE;
  }
  if (timeout)
    ++wakeup->blocked;
  return 0;
}
