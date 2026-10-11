// Host checks for the finalize-time CRC cache. Synthetic keys do not prove SD
// timestamps; the board record covers real finalize and OPEN latency.
#include "crc_cache.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>

using aq::storage::crc_path_hash;
using aq::storage::crc_stat_key;
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
// Mirrors finalize (remember_crc on the ready path after the .partial rename)
// and OPEN (crc_key on the same path). Host rename(2), not FatFs f_rename.
void key_after_rename(const std::string &directory) {
  const std::string partial = directory + "/a.parquet.partial";
  const std::string ready = directory + "/a.parquet";
  FILE *file = std::fopen(partial.c_str(), "wb");
  assert(file && std::fwrite("PAR1xxxxPAR1", 1, 12, file) == 12);
  assert(std::fclose(file) == 0);
  CrcKey before{};
  assert(crc_stat_key(partial.c_str(), before) && before.size == 12);
  assert(std::rename(partial.c_str(), ready.c_str()) == 0);

  CrcKey stored{}, opened{}, stale{};
  assert(crc_stat_key(ready.c_str(), stored));
  assert(stored.size == before.size && stored.mtime == before.mtime);
  CrcCache<4> cache;
  cache.insert(stored, 0x414fa339U);
  std::uint32_t crc = 0;
  assert(crc_stat_key(ready.c_str(), opened));
  assert(cache.lookup(opened, crc) && crc == 0x414fa339U);
  // A key remembered before the rename names the old path and never hits.
  assert(!cache.lookup(before, crc) && cache.size() == 1);
  assert(!crc_stat_key(partial.c_str(), stale));
  assert(!crc_stat_key(nullptr, stale));
}
} // namespace

int main(int argc, char **argv) {
  assert(argc == 2);
  hit_and_miss();
  invalidation();
  lru_eviction();
  clear_all();
  key_after_rename(argv[1]);
  return 0;
}
