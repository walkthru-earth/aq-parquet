#pragma once
#include <cstdint>
#include <mutex>
using TickType_t = std::uint32_t;
constexpr int pdTRUE = 1, pdPASS = 1;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
#define pdMS_TO_TICKS(value) (value)
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED                                           \
  {                                                                            \
  }
#define portENTER_CRITICAL(mutex) (mutex)->lock()
#define portEXIT_CRITICAL(mutex) (mutex)->unlock()
