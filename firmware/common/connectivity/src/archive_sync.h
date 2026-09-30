#pragma once

#include "ble_sync.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace aqsync {
using FileEmitter = void (*)(void *context, const char *name,
                             std::uint32_t bytes);

// Board/archive policy: enumerating and resolving the mounted archive,
// checking finalized Parquet files, storage statistics and its bus mutex.
// All callbacks run only on the caller's storage worker. The module never
// mounts, formats, repairs, deletes or writes files. list/resolve/finalized and
// both capacity callbacks are required. lock/unlock are a pair: provide both
// when the filesystem shares a bus, or neither when no bus mutex is needed.
// listed/log are optional. Contexts must outlive the session.
struct ArchiveHooks {
  void *context = nullptr;
  unsigned (*list)(void *, FileEmitter, void *) = nullptr;
  bool (*resolve)(void *, const char *, char *, std::size_t) = nullptr;
  bool (*finalized)(void *, const char *, std::uint32_t &,
                    std::uint32_t &) = nullptr;
  std::uint32_t (*total_kib)(void *) = nullptr;
  std::uint32_t (*used_kib)(void *) = nullptr;
  void (*listed)(void *) = nullptr;
  void (*lock)(void *) = nullptr;
  void (*unlock)(void *) = nullptr;
  void (*log)(void *, const char *) = nullptr;
};

// Transport adapters are independent of the filesystem and may be faked in
// host tests. Generations increment whenever the corresponding peer changes.
// BLE payloads are additionally capped at the GATT 512-byte attribute limit;
// all links are bounded to the 1024-byte protocol capacity. generation,
// payload_max, respond and error are required; progress is optional.
struct ArchiveTransport {
  void *context = nullptr;
  std::uint32_t (*generation)(void *, ble::Link) = nullptr;
  std::uint16_t (*payload_max)(void *, ble::Link) = nullptr;
  bool (*respond)(void *, ble::Link, const std::uint8_t *,
                  std::size_t) = nullptr;
  bool (*error)(void *, ble::Link, ble::Op, ble::Error, const char *) = nullptr;
  void (*progress)(void *) = nullptr;
};

// One immutable file handle shared between BLE/LAN, bound to the link and
// connection generation that opened it. Construct once, then call exclusively
// from the same storage worker that owns the archive. Radio callbacks must
// enqueue ControlRequest copies; they must never call this class directly.
class ArchiveSession {
public:
  ArchiveSession(const ArchiveHooks &archive,
                 const ArchiveTransport &transport);
  ArchiveSession(const ArchiveSession &) = delete;
  ArchiveSession &operator=(const ArchiveSession &) = delete;

  // Returns true for LIST/OPEN/READ/CLOSE (including error responses), false
  // for other operations so the board's clock/config/logger handler can run.
  bool handle(const ble::ControlRequest &request, bool storage_ready);
  void reconcile();
  void close();

private:
  struct OpenFile {
    FILE *file = nullptr;
    std::uint16_t handle = 0;
    std::uint32_t size = 0;
    std::uint32_t crc = 0;
    std::uint32_t generation = 0;
    ble::Link link = ble::Link::Ble;
  };
  struct ListContext {
    ArchiveSession *session;
    std::uint16_t count = 0;
    bool ok = true;
    bool unsendable = false;
  };
  static constexpr std::size_t kPayloadCapacity = 1024;
  ArchiveHooks archive_;
  ArchiveTransport transport_;
  OpenFile open_;
  std::uint16_t next_handle_ = 1;
  ble::Link request_link_ = ble::Link::Ble;

  std::size_t payload_max() const;
  bool respond(const std::uint8_t *frame, std::size_t length);
  bool error(ble::Op op, ble::Error code, const char *detail = nullptr);
  bool handle_matches(std::uint16_t file_handle) const;
  static void emit_file(void *context, const char *name, std::uint32_t bytes);
  void list();
  void open(const std::uint8_t *name_bytes, std::size_t name_length);
  void read(std::uint16_t file_handle, std::uint32_t offset,
            std::uint32_t length);
  void close(std::uint16_t file_handle);
};
} // namespace aqsync
