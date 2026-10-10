#include "log_persist.h"

#include <esp_attr.h>
#include <esp_system.h>
#include <sdkconfig.h>

#include <cstdint>

#if CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY
#include <esp_psram.h>

namespace {
constexpr DebugLog::Layout kReserved{CONFIG_AQ_LOG_PERSIST_RING_BYTES,
                                     CONFIG_AQ_LOG_PERSIST_EVENT_SLOTS};
EXT_RAM_NOINIT_ATTR alignas(4) std::uint8_t
    persistent_block[DebugLog::persistent_bytes(kReserved)];
} // namespace
#endif

namespace aq::log {
DebugLog::Layout default_layout() {
  return {CONFIG_AQ_LOG_PERSIST_RING_BYTES, CONFIG_AQ_LOG_PERSIST_EVENT_SLOTS};
}

bool attach_persistent(const DebugLog::Layout &layout) {
#if CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY
  if (!esp_psram_is_initialized()) {
    aqlog.printf("LOG ring=internal bytes=%u reason=no-psram\n",
                 unsigned(aqlog.ring_bytes()));
    return false;
  }
  if (DebugLog::persistent_bytes(layout) > sizeof(persistent_block)) {
    aqlog.printf("LOG ring=internal bytes=%u reason=layout-too-large\n",
                 unsigned(aqlog.ring_bytes()));
    return false;
  }
  const esp_reset_reason_t reset = esp_reset_reason();
  // PSRAM loses its contents without power; never trust it after one.
  const bool cold = reset == ESP_RST_POWERON || reset == ESP_RST_BROWNOUT ||
                    reset == ESP_RST_UNKNOWN;
  const bool kept =
      aqlog.attach(persistent_block, sizeof(persistent_block), layout, cold);
  aqlog.printf("LOG ring=psram bytes=%u events=%u boot_seq=%u kept=%d "
               "retained=%u reset=%d\n",
               unsigned(aqlog.ring_bytes()), unsigned(layout.event_slots),
               unsigned(aqlog.boot_seq()), kept ? 1 : 0,
               unsigned(aqlog.retained()), int(reset));
  return aqlog.persistent();
#else
  (void)layout;
  aqlog.printf("LOG ring=internal bytes=%u reason=no-noinit\n",
               unsigned(aqlog.ring_bytes()));
  return false;
#endif
}
} // namespace aq::log
