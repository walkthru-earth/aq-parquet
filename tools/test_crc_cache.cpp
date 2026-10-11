// Host checks for the finalize-time CRC cache. Synthetic keys do not prove SD
// timestamps; the board record covers real finalize and OPEN latency.
#include "crc_cache.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

using aq::storage::crc_path_hash;
using aq::storage::CrcCache;
using aq::storage::CrcKey;

namespace {
CrcKey key(const char *path, std::uint32_t size, std::int64_t mtime) {
  return CrcKey{crc_path_hash(path), mtime, size};
}

void hit_and_miss() {
  CrcCache<4> cache;
  std::uint32_t crc = 0;
  assert(!cache.lookup(key("a.parquet", 100, 1), crc));
  cache.insert(key("a.parquet", 100, 1), 0xcbf43926U);
  assert(cache.lookup(key("a.parquet", 100, 1), crc) && crc == 0xcbf43926U);
  assert(!cache.lookup(key("b.parquet", 100, 1), crc));
  assert(cache.size() == 1);
}

void invalidation() {
  CrcCache<4> cache;
  std::uint32_t crc = 0;
  cache.insert(key("a.parquet", 100, 1), 1);
  // A changed size or mtime means the file is no longer the finalized one.
  assert(!cache.lookup(key("a.parquet", 101, 1), crc));
  assert(cache.size() == 0);
  cache.insert(key("a.parquet", 100, 1), 1);
  assert(!cache.lookup(key("a.parquet", 100, 2), crc));
  assert(cache.size() == 0);
  cache.insert(key("a.parquet", 100, 1), 1);
  cache.insert(key("a.parquet", 100, 3), 2);
  assert(cache.size() == 1);
  assert(cache.lookup(key("a.parquet", 100, 3), crc) && crc == 2);
  cache.erase(crc_path_hash("a.parquet"));
  assert(cache.size() == 0);
}

void lru_eviction() {
  CrcCache<3> cache;
  std::uint32_t crc = 0;
  cache.insert(key("a", 1, 1), 10);
  cache.insert(key("b", 1, 1), 11);
  cache.insert(key("c", 1, 1), 12);
  assert(cache.lookup(key("a", 1, 1), crc) && crc == 10);
  cache.insert(key("d", 1, 1), 13); // evicts b, the least recently used
  assert(!cache.lookup(key("b", 1, 1), crc));
  assert(cache.lookup(key("a", 1, 1), crc) && crc == 10);
  assert(cache.lookup(key("c", 1, 1), crc) && crc == 12);
  assert(cache.lookup(key("d", 1, 1), crc) && crc == 13);
  assert(cache.size() == 3);
}

void clear_all() {
  CrcCache<64> cache;
  char path[16];
  for (unsigned i = 0; i < 80; ++i) {
    std::snprintf(path, sizeof(path), "f%u", i);
    cache.insert(key(path, i + 12, i), i);
  }
  assert(cache.size() == 64);
  std::uint32_t crc = 0;
  assert(!cache.lookup(key("f0", 12, 0), crc));
  assert(cache.lookup(key("f79", 91, 79), crc) && crc == 79);
  cache.clear();
  assert(cache.size() == 0);
  assert(!cache.lookup(key("f79", 91, 79), crc));
}
} // namespace

int main() {
  hit_and_miss();
  invalidation();
  lru_eviction();
  clear_all();
  return 0;
}
