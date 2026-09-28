#include <Arduino.h>
#include <cstddef>
#include <cstdint>
#include <esp_heap_caps.h>
#include <esp_psram.h>
#include <initializer_list>

// Physical-board qualification fixture, not a logger/framework trial. Only
// writes a region it allocated; never touches card data, pins or flash fuses.
namespace {
constexpr std::size_t kProbeBytes = 512 * 1024;
bool memory_ok = false;

bool probe() {
  auto *memory = static_cast<volatile std::uint32_t *>(
      heap_caps_malloc(kProbeBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!memory)
    return false;
  bool ok = true;
  for (std::uint32_t pattern : {0xa5a55a5aU, 0x5a5aa5a5U, 0U, 0xffffffffU}) {
    for (std::size_t i = 0; i < kProbeBytes / sizeof(*memory); ++i)
      memory[i] = pattern ^ static_cast<std::uint32_t>(i * 2654435761U);
    for (std::size_t i = 0; i < kProbeBytes / sizeof(*memory); ++i)
      if (memory[i] !=
          (pattern ^ static_cast<std::uint32_t>(i * 2654435761U))) {
        ok = false;
        break;
      }
  }
  heap_caps_free(const_cast<std::uint32_t *>(memory));
  return ok;
}
} // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.printf("AQ PSRAM chip=%s revision=%u flash_bytes=%lu initialized=%u "
                "physical_bytes=%lu heap_bytes=%lu free_bytes=%lu\n",
                ESP.getChipModel(), unsigned(ESP.getChipRevision()),
                static_cast<unsigned long>(ESP.getFlashChipSize()),
                esp_psram_is_initialized(),
                static_cast<unsigned long>(esp_psram_get_size()),
                static_cast<unsigned long>(ESP.getPsramSize()),
                static_cast<unsigned long>(ESP.getFreePsram()));
  memory_ok = esp_psram_is_initialized() &&
              esp_psram_get_size() == 8 * 1024 * 1024 && probe();
  Serial.printf("AQ PSRAM probe_bytes=%u patterns=4 ok=%u\n",
                unsigned(kProbeBytes), memory_ok);
}

void loop() {
  Serial.printf("AQ PSRAM alive=1 ok=%u free_bytes=%lu\n", memory_ok,
                static_cast<unsigned long>(ESP.getFreePsram()));
  delay(5000);
}
