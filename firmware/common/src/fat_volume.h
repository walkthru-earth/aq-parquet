#pragma once

#include <diskio_sdmmc.h>
#include <esp_vfs_fat.h>
#include <ff.h>
#include <sdmmc_cmd.h>

#include <cstdint>

namespace aq::storage {

// Bytes per FAT allocation unit of the volume mounted from `card`, or 0 when
// unknown. Every file grows in whole clusters, so the space governor logs it
// at mount. The caller serializes card access.
inline std::uint32_t fat_cluster_bytes(sdmmc_card_t *card) {
  if (!card)
    return 0;
  const BYTE drive_number = ff_diskio_get_pdrv_card(card);
  if (drive_number == 0xFF)
    return 0;
  const char drive[] = {static_cast<char>('0' + drive_number), ':', '\0'};
  DWORD free_clusters = 0;
  FATFS *volume = nullptr;
  if (f_getfree(drive, &free_clusters, &volume) != FR_OK || !volume)
    return 0;
#if FF_MAX_SS != FF_MIN_SS
  const std::uint32_t sector_bytes = volume->ssize;
#else
  const std::uint32_t sector_bytes = FF_MAX_SS;
#endif
  return static_cast<std::uint32_t>(volume->csize) * sector_bytes;
}

// Fresh free space of the volume at `base`. FatFs keeps the free cluster
// count current as files grow; a remount re-reads it after an external edit.
inline bool fat_free_bytes(const char *base, std::uint64_t &free_bytes) {
  std::uint64_t total = 0, free = 0;
  if (esp_vfs_fat_info(base, &total, &free) != ESP_OK || free > total)
    return false;
  free_bytes = free;
  return true;
}

} // namespace aq::storage
