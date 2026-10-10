#include "log_persist.h"

#include <esp_attr.h>
#include <esp_system.h>
#include <sdkconfig.h>

#include <cstdint>

#if CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY
#include <esp_psram.h>
#include <hal/cache_hal.h>

namespace {
constexpr DebugLog::Layout kReserved{CONFIG_AQ_LOG_PERSIST_RING_BYTES,
                                     CONFIG_AQ_LOG_PERSIST_EVENT_SLOTS};
// PSRAM init on the ESP32-S3 (quad and octal) writes a probe word to
// physical address 0 on every boot, which is where the no-init segment
// starts. Keep the block one cache line past that.
constexpr std::size_t kProbeGuardBytes = 64;
EXT_RAM_NOINIT_ATTR alignas(64) std::uint8_t
    persistent_storage[kProbeGuardBytes +
                       DebugLog::persistent_bytes(kReserved)];
std::uint8_t *const persistent_block = persistent_storage + kProbeGuardBytes;
constexpr std::size_t kPersistentBytes = DebugLog::persistent_bytes(kReserved);

// PSRAM sits behind the write-back data cache. Without this, a reset that
// skips esp_restart's teardown (watchdog, panic, reset pin) can lose the
// header and the newest lines while they are still dirty in the cache.
void writeback(const void *address, std::size_t bytes) {
  cache_hal_writeback_addr(reinterpret_cast<std::uint32_t>(address),
                           static_cast<std::uint32_t>(bytes));
}
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
  if (DebugLog::persistent_bytes(layout) > kPersistentBytes) {
    aqlog.printf("LOG ring=internal bytes=%u reason=layout-too-large\n",
                 unsigned(aqlog.ring_bytes()));
    return false;
  }
  const esp_reset_reason_t reset = esp_reset_reason();
  // PSRAM loses its contents without power; never trust it after one.
  const bool cold = reset == ESP_RST_POWERON || reset == ESP_RST_BROWNOUT ||
                    reset == ESP_RST_UNKNOWN;
  const bool kept =
      aqlog.attach(persistent_block, kPersistentBytes, layout, cold, writeback);
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
