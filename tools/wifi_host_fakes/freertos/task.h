#pragma once
#include "FreeRTOS.h"
inline int fake_task_result = pdPASS;
inline unsigned fake_task_stack = 0;
inline int fake_task_priority = -1;
inline int xTaskCreate(void (*)(void *), const char *, unsigned stack, void *,
                       int priority, void *) {
  fake_task_stack = stack;
  fake_task_priority = priority;
  return fake_task_result;
}
inline void vTaskDelay(TickType_t) {}
