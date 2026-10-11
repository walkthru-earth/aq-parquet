#include "power.h"
#include "debug_log.h"

#include <esp_pm.h>
#include <sdkconfig.h>

namespace aq::power {
bool configure_dfs() {
#if CONFIG_PM_ENABLE
  esp_pm_config_t config{};
  config.max_freq_mhz = CONFIG_AQ_PM_DFS_MAX_MHZ;
  config.min_freq_mhz = CONFIG_AQ_PM_DFS_MIN_MHZ;
  config.light_sleep_enable = false;
  const esp_err_t result = esp_pm_configure(&config);
  aqlog.printf("POWER DFS pm=true min_mhz=%d max_mhz=%d light_sleep=false "
               "code=%d\n",
               config.min_freq_mhz, config.max_freq_mhz, result);
  return result == ESP_OK;
#else
  aqlog.println("POWER DFS pm=false");
  return false;
#endif
}
} // namespace aq::power
