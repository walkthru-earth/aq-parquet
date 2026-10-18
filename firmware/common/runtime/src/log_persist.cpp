#include "log_persist.h"

#include <esp_attr.h>
#include <esp_system.h>
#include <sdkconfig.h>

#include <array>
#include <cstdint>

#include "coredump_report.h"

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#include <algorithm>
#include <esp_core_dump.h>
#include <esp_partition.h>
#include <nvs.h>

namespace {
namespace cd = aq::coredump;

// Own namespace and key, so no existing NVS namespace, key or type changes.
constexpr const char *kDumpNamespace = "aqdump";
constexpr const char *kDumpKey = "reported";

std::expected<cd::Identity, cd::SourceError> dump_identity(void *) {
  std::size_t address = 0;
  std::size_t size = 0;
  const esp_err_t found = esp_core_dump_image_get(&address, &size);
  if (found == ESP_ERR_NOT_FOUND)
    return std::unexpected(cd::SourceError::Absent);
  if (found == ESP_ERR_INVALID_SIZE)
    return std::unexpected(cd::SourceError::InvalidSize);
  if (found != ESP_OK)
    return std::unexpected(cd::SourceError::ReadFailed);
  if (size < cd::kChecksumBytes)
    return std::unexpected(cd::SourceError::InvalidSize);
  const esp_partition_t *partition = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
  if (!partition)
    return std::unexpected(cd::SourceError::ReadFailed);
  // ESP-IDF stores the image SHA-256 in its last bytes.
  cd::Identity identity{.bytes = static_cast<std::uint32_t>(size)};
  if (esp_partition_read(partition, size - cd::kChecksumBytes,
                         identity.checksum.data(),
                         identity.checksum.size()) != ESP_OK)
    return std::unexpected(cd::SourceError::ReadFailed);
  return identity;
}

std::expected<void, cd::SourceError> dump_verify(void *) {
  switch (esp_core_dump_image_check()) {
  case ESP_OK:
    return {};
  case ESP_ERR_INVALID_CRC:
  case ESP_ERR_INVALID_SIZE:
    return std::unexpected(cd::SourceError::Corrupt);
  case ESP_ERR_NOT_FOUND:
    return std::unexpected(cd::SourceError::Absent);
  default:
    return std::unexpected(cd::SourceError::ReadFailed);
  }
}

std::expected<cd::Summary, cd::SourceError> dump_summary(void *) {
  esp_core_dump_summary_t raw{};
  if (esp_core_dump_get_summary(&raw) != ESP_OK)
    return std::unexpected(cd::SourceError::ReadFailed);
  cd::Summary summary{.pc = raw.exc_pc};
  std::copy_n(raw.exc_task, std::min(sizeof(raw.exc_task), summary.task.size()),
              summary.task.begin());
#if CONFIG_IDF_TARGET_ARCH_XTENSA
  summary.exc_cause = raw.ex_info.exc_cause;
  summary.exc_vaddr = raw.ex_info.exc_vaddr;
#else
  summary.exc_cause = raw.ex_info.mcause;
  summary.exc_vaddr = raw.ex_info.mtval;
#endif
  // app_elf_sha256 is NUL terminated hex text.
  std::copy_n(raw.app_elf_sha256,
              std::min(sizeof(raw.app_elf_sha256) - 1, summary.elf.size()),
              summary.elf.begin());
  return summary;
}

std::expected<cd::Record, cd::StoreError> record_load(void *) {
  nvs_handle_t handle = 0;
  const esp_err_t opened = nvs_open(kDumpNamespace, NVS_READONLY, &handle);
  if (opened == ESP_ERR_NVS_NOT_FOUND)
    return std::unexpected(cd::StoreError::Missing);
  if (opened != ESP_OK)
    return std::unexpected(cd::StoreError::ReadFailed);
  cd::Record record{};
  std::size_t length = record.size();
  const esp_err_t read = nvs_get_blob(handle, kDumpKey, record.data(), &length);
  nvs_close(handle);
  if (read == ESP_ERR_NVS_NOT_FOUND)
    return std::unexpected(cd::StoreError::Missing);
  // A blob of another length is not ours. Version 0 makes decode reject it.
  if (read == ESP_ERR_NVS_INVALID_LENGTH ||
      (read == ESP_OK && length != record.size()))
    return cd::Record{};
  if (read != ESP_OK)
    return std::unexpected(cd::StoreError::ReadFailed);
  return record;
}

std::expected<void, cd::StoreError>
record_save(void *, std::span<const std::uint8_t, cd::kRecordBytes> record) {
  nvs_handle_t handle = 0;
  if (nvs_open(kDumpNamespace, NVS_READWRITE, &handle) != ESP_OK)
    return std::unexpected(cd::StoreError::WriteFailed);
  const bool ok =
      nvs_set_blob(handle, kDumpKey, record.data(), record.size()) == ESP_OK &&
      nvs_commit(handle) == ESP_OK;
  nvs_close(handle);
  if (!ok)
    return std::unexpected(cd::StoreError::WriteFailed);
  return {};
}
} // namespace
#endif

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

void report_coredump() {
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
  const cd::Source source{.context = nullptr,
                          .identity = dump_identity,
                          .verify = dump_verify,
                          .summary = dump_summary};
  const cd::Store store{
      .context = nullptr, .load = record_load, .save = record_save};
  std::array<char, cd::kLineBytes> line{};
  const cd::Report result = cd::report(source, store, line);
  if (result.length > 0)
    aqlog.printf("%s", line.data());
#endif
}
} // namespace aq::log
