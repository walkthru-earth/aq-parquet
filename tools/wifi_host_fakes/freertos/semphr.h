#pragma once
#include "FreeRTOS.h"
#include <chrono>
#include <condition_variable>
#include <mutex>
// A mutex semaphore is a timed_mutex; a binary semaphore is a flag guarded by
// a condition variable, as in FreeRTOS (Give sets it, Take waits and clears).
struct FakeSemaphore {
  bool binary = false;
  std::timed_mutex mutex;
  std::mutex flag_mutex;
  std::condition_variable flag_changed;
  bool flag = false;
};
using SemaphoreHandle_t = FakeSemaphore *;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new FakeSemaphore; }
inline SemaphoreHandle_t xSemaphoreCreateBinary() {
  auto *semaphore = new FakeSemaphore;
  semaphore->binary = true;
  return semaphore;
}
inline int xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks) {
  if (semaphore->binary) {
    std::unique_lock<std::mutex> lock(semaphore->flag_mutex);
    const auto ready = [semaphore] { return semaphore->flag; };
    if (ticks == portMAX_DELAY)
      semaphore->flag_changed.wait(lock, ready);
    else if (!semaphore->flag_changed.wait_for(
                 lock, std::chrono::milliseconds(ticks), ready))
      return 0;
    semaphore->flag = false;
    return pdTRUE;
  }
  if (ticks == portMAX_DELAY) {
    semaphore->mutex.lock();
    return pdTRUE;
  }
  return semaphore->mutex.try_lock_for(std::chrono::milliseconds(ticks))
             ? pdTRUE
             : 0;
}
inline void xSemaphoreGive(SemaphoreHandle_t semaphore) {
  if (semaphore->binary) {
    {
      std::lock_guard<std::mutex> lock(semaphore->flag_mutex);
      semaphore->flag = true;
    }
    semaphore->flag_changed.notify_all();
    return;
  }
  semaphore->mutex.unlock();
}
inline void vSemaphoreDelete(SemaphoreHandle_t semaphore) { delete semaphore; }
