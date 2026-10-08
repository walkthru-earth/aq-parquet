#pragma once
#include "FreeRTOS.h"
inline int xTaskCreate(void (*)(void *), const char *, unsigned, void *, int,
                       void *) {
  return pdPASS;
}
inline void vTaskDelay(TickType_t) {}
