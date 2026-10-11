#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>

// Finalize-time CRC cache for archive OPEN. Finalized files are immutable, so
// the CRC computed after the .partial rename stays valid while the path, size
// and modification time still match. Any mismatch evicts the entry and the
// caller recomputes. Pure and allocation-free so the host test exercises it
// unchanged. The logger places one instance in PSRAM and touches it only from
// its storage worker, so it needs no lock.
namespace aq {
namespace storage {
struct CrcKey {
  std::uint64_t path_hash = 0;
  std::int64_t mtime = 0;
  std::uint32_t size = 0;
};

// FNV-1a 64. Collisions are guarded by the size and mtime match.
inline std::uint64_t crc_path_hash(const char *path) {
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (; path && *path; ++path) {
    hash ^= static_cast<unsigned char>(*path);
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

template <std::size_t Capacity> class CrcCache {
  static_assert(Capacity > 0, "CRC cache needs at least one entry");

public:
  static constexpr std::size_t capacity = Capacity;

  bool lookup(const CrcKey &key, std::uint32_t &crc) {
    Entry *entry = find(key.path_hash);
    if (!entry)
      return false;
    if (entry->size != key.size || entry->mtime != key.mtime) {
      *entry = Entry{};
      return false;
    }
    entry->used = ++clock_;
    crc = entry->crc;
    return true;
  }

  void insert(const CrcKey &key, std::uint32_t crc) {
    Entry *entry = find(key.path_hash);
    if (!entry) {
      entry = &entries_[0];
      for (auto &candidate : entries_) {
        if (!candidate.valid) {
          entry = &candidate;
          break;
        }
        if (candidate.used < entry->used)
          entry = &candidate;
      }
    }
    *entry = Entry{key.path_hash, key.mtime, key.size, crc, ++clock_, true};
  }

  void erase(std::uint64_t path_hash) {
    if (Entry *entry = find(path_hash))
      *entry = Entry{};
  }

  void clear() { std::fill(std::begin(entries_), std::end(entries_), Entry{}); }

  std::size_t size() const {
    return static_cast<std::size_t>(
        std::count_if(std::begin(entries_), std::end(entries_),
                      [](const Entry &entry) { return entry.valid; }));
  }

private:
  struct Entry {
    std::uint64_t path_hash = 0;
    std::int64_t mtime = 0;
    std::uint32_t size = 0, crc = 0;
    std::uint64_t used = 0;
    bool valid = false;
  };

  Entry *find(std::uint64_t path_hash) {
    Entry *entry = std::find_if(std::begin(entries_), std::end(entries_),
                                [path_hash](const Entry &candidate) {
                                  return candidate.valid &&
                                         candidate.path_hash == path_hash;
                                });
    return entry == std::end(entries_) ? nullptr : entry;
  }

  Entry entries_[Capacity]{};
  std::uint64_t clock_ = 0;
};
} // namespace storage
} // namespace aq
