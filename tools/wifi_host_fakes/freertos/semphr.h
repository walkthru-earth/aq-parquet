#pragma once
#include "FreeRTOS.h"
#include <chrono>
using SemaphoreHandle_t = std::timed_mutex *;
inline SemaphoreHandle_t xSemaphoreCreateMutex() {
  return new std::timed_mutex;
}
inline int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t ticks) {
  if (ticks == portMAX_DELAY) {
    mutex->lock();
    return pdTRUE;
  }
  return mutex->try_lock_for(std::chrono::milliseconds(ticks)) ? pdTRUE : 0;
}
inline void xSemaphoreGive(SemaphoreHandle_t mutex) { mutex->unlock(); }
