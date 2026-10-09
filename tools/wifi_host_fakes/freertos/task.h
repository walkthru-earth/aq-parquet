#pragma once
#include "FreeRTOS.h"
inline int fake_task_result = pdPASS;
inline int xTaskCreate(void (*)(void *), const char *, unsigned, void *, int,
                       void *) {
  return fake_task_result;
}
inline void vTaskDelay(TickType_t) {}
