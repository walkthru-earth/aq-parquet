#pragma once
#include <mutex>

// Host exclusion models the lock contract, not FreeRTOS scheduling/radio tasks.
using portMUX_TYPE = std::recursive_mutex;
#define portMUX_INITIALIZER_UNLOCKED                                           \
  {                                                                            \
  }
inline void portENTER_CRITICAL(portMUX_TYPE *mutex) { mutex->lock(); }
inline void portEXIT_CRITICAL(portMUX_TYPE *mutex) { mutex->unlock(); }
