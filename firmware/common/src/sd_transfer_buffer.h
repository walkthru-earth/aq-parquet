#pragma once

#include <esp_heap_caps.h>

#include <cstddef>

namespace opensensor::storage {

// Reserve before starting radios. ESP-IDF borrows this heap-owned buffer from
// sdmmc_host_t and queries its allocated size; a static array is not suitable.
// The board keeps it alive while mounted. Its single archive worker serializes
// card transfers, so one buffer serves reads and writes without a second lock.
class SdTransferBuffer {
public:
  static constexpr std::size_t kSectorBytes = 512;
  static constexpr std::size_t kSectors = 8;
  static constexpr std::size_t kBytes = kSectorBytes * kSectors;

  SdTransferBuffer() = default;
  ~SdTransferBuffer() { release(); }
  SdTransferBuffer(const SdTransferBuffer &) = delete;
  SdTransferBuffer &operator=(const SdTransferBuffer &) = delete;

  bool reserve() {
    if (!data_)
      data_ = heap_caps_malloc(kBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    return data_ != nullptr;
  }

  void release() {
    heap_caps_free(data_);
    data_ = nullptr;
  }

  void *data() const { return data_; }

private:
  void *data_ = nullptr;
};

} // namespace opensensor::storage
