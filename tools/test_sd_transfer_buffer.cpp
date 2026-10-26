#include <sd_transfer_buffer.h>

#include <cassert>
#include <cstdlib>
#include <type_traits>

namespace {
bool reject_allocations = false;
unsigned allocations = 0, releases = 0;
void *active = nullptr;
} // namespace

void *heap_caps_malloc(std::size_t bytes, std::uint32_t caps) {
  ++allocations;
  assert(bytes == 4096);
  assert(caps == (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  if (reject_allocations)
    return nullptr;
  assert(!active);
  active = std::malloc(bytes);
  return active;
}

void heap_caps_free(void *data) {
  if (!data)
    return;
  assert(data == active);
  ++releases;
  std::free(data);
  active = nullptr;
}

int main() {
  using Buffer = opensensor::storage::SdTransferBuffer;
  static_assert(!std::is_copy_constructible_v<Buffer>);
  static_assert(!std::is_copy_assignable_v<Buffer>);
  static_assert(Buffer::kBytes == Buffer::kSectors * 512);
  {
    Buffer buffer;
    reject_allocations = true;
    assert(!buffer.reserve() && !buffer.data());
    assert(allocations == 1 && releases == 0);
    reject_allocations = false;
    assert(buffer.reserve());
    void *reserved = buffer.data();
    reject_allocations = true; // Wi-Fi load cannot allocate another DMA buffer.
    assert(buffer.reserve() && buffer.data() == reserved);
    assert(allocations == 2);
    buffer.release(); // The board releases its reservation if mount fails.
    assert(!buffer.data() && releases == 1);
    buffer.release();
    assert(releases == 1);
    reject_allocations = false;
    assert(buffer.reserve()); // Retrying a failed mount can reserve again.
  }
  assert(!active && allocations == 3 && releases == 2);
}
