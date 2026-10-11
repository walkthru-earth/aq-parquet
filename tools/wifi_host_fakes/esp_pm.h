#pragma once
#include "esp_err.h"
enum esp_pm_lock_type_t {
  ESP_PM_CPU_FREQ_MAX,
  ESP_PM_APB_FREQ_MAX,
  ESP_PM_NO_LIGHT_SLEEP
};
struct esp_pm_lock {
  esp_pm_lock_type_t type;
  int held = 0;
};
using esp_pm_lock_handle_t = esp_pm_lock *;
// ESP_ERR_NOT_SUPPORTED stands in for an image without CONFIG_PM_ENABLE.
inline esp_err_t fake_pm_create_result = ESP_OK;
inline esp_pm_lock fake_pm_lock{ESP_PM_CPU_FREQ_MAX};
inline esp_err_t esp_pm_lock_create(esp_pm_lock_type_t type, int, const char *,
                                    esp_pm_lock_handle_t *handle) {
  if (fake_pm_create_result != ESP_OK)
    return fake_pm_create_result;
  fake_pm_lock = {type};
  *handle = &fake_pm_lock;
  return ESP_OK;
}
inline esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t handle) {
  ++handle->held;
  return ESP_OK;
}
inline esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t handle) {
  if (handle->held == 0)
    return ESP_ERR_INVALID_STATE;
  --handle->held;
  return ESP_OK;
}
