#pragma once

#include "ble_sync.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace aqsync {
using FileEmitter = void (*)(void *context, const char *name,
                             std::uint32_t bytes);

// Result of the finalized check behind OPEN. cached is true when the CRC came
// from the owner's finalize-time cache instead of a full read.
struct FileCheck {
  std::uint32_t size = 0;
  std::uint32_t crc = 0;
  bool cached = false;
};

struct ListResult {
  unsigned partials = 0;
  bool complete = false;
};

// Board/archive policy: enumerating and resolving the mounted archive,
// checking finalized Parquet files, storage statistics and its bus mutex.
// All callbacks run only on the caller's storage worker. The module never
// mounts, formats, repairs, deletes or writes files. list/resolve/finalized and
// both capacity callbacks are required. lock/unlock are a pair: provide both
// when the filesystem shares a bus, or neither when no bus mutex is needed.
// listed/log/read_buffer/now_us are optional. Contexts must outlive the
// session.
struct ArchiveHooks {
  void *context = nullptr;
  // complete is true only after every configured archive tree was read. FILE
  // frames already emitted during a failed walk are an incomplete snapshot.
  ListResult (*list)(void *, FileEmitter, void *) = nullptr;
  bool (*resolve)(void *, const char *, char *, std::size_t) = nullptr;
  // Checks the file OPEN just opened (positioned at 0) and fills size and
  // CRC-32. Called without the lock held; it takes lock/unlock itself around
  // each filesystem call. The session repositions the stream before reads.
  bool (*finalized)(void *, const char *path, FILE *file,
                    FileCheck &check) = nullptr;
  std::uint32_t (*total_kib)(void *) = nullptr;
  std::uint32_t (*used_kib)(void *) = nullptr;
  void (*listed)(void *) = nullptr;
  void (*lock)(void *) = nullptr;
  void (*unlock)(void *) = nullptr;
  void (*log)(void *, const char *) = nullptr;
  // Stable kReadBufferBytes stdio buffer for read slot 0..kReadHandles-1, or
  // nullptr to keep the C library default.
  void *(*read_buffer)(void *, std::size_t slot) = nullptr;
  // Monotonic microseconds for the OPEN latency log field.
  std::int64_t (*now_us)(void *) = nullptr;
};

// Transport adapters are independent of the filesystem and may be faked in
// host tests. Callbacks receive link and peer; respond/error also receive the
// expected generation and must refuse delivery to a replacement connection.
// Generations increment whenever the corresponding peer changes.
// BLE payloads are additionally capped at the GATT 512-byte attribute limit;
// all links are bounded to the 1024-byte protocol capacity. generation,
// payload_max, respond and error are required; progress is optional.
struct ArchiveTransport {
  void *context = nullptr;
  std::uint32_t (*generation)(void *, ble::Link, std::uint8_t) = nullptr;
  std::uint16_t (*payload_max)(void *, ble::Link, std::uint8_t) = nullptr;
  bool (*respond)(void *, ble::Link, std::uint8_t, std::uint32_t,
                  const std::uint8_t *, std::size_t) = nullptr;
  bool (*error)(void *, ble::Link, std::uint8_t, std::uint32_t, ble::Op,
                ble::Error, const char *) = nullptr;
  void (*progress)(void *) = nullptr;
};

// Independent immutable logical file handles for BLE and each LAN peer, bound
// to their connection generations. Each open handle keeps one read-only FILE
// until CLOSE, a re-OPEN on its slot, a generation change, close() or
// release_files(), so READ windows seek instead of reopening. The mount must
// budget kReadHandles descriptors for these slots (archive_files.h).
// Construct once, then call exclusively from the storage worker that owns the
// archive. Radio callbacks must
// enqueue ControlRequest copies; they must never call this class directly.
class ArchiveSession {
public:
  static constexpr std::size_t kReadHandles = 1 + ble::kMaxLanClients;
  static constexpr std::size_t kReadBufferBytes = 4096;

  ArchiveSession(const ArchiveHooks &archive,
                 const ArchiveTransport &transport);
  ~ArchiveSession() { release_files(); }
  ArchiveSession(const ArchiveSession &) = delete;
  ArchiveSession &operator=(const ArchiveSession &) = delete;

  // Returns true for LIST/OPEN/READ/CLOSE (including error responses), false
  // for other operations so the board's clock/config/logger handler can run.
  bool handle(const ble::ControlRequest &request, bool storage_ready);
  void reconcile();
  // Drops every logical handle and closes its descriptor.
  void close();
  // Closes descriptors but keeps logical handles. The next READ reopens the
  // path. Call before a storage remount.
  void release_files();

private:
  struct OpenFile {
    char path[416]{};
    FILE *file = nullptr;
    std::uint16_t handle = 0;
    std::uint32_t size = 0;
    std::uint32_t crc = 0;
    std::uint32_t generation = 0;
  };
  struct ListContext {
    ArchiveSession *session;
    std::uint32_t count = 0;
    bool ok = true;
    bool aborted = false;
    const char *failure_detail = nullptr;
  };
  static constexpr std::size_t kPayloadCapacity = 1024;
  ArchiveHooks archive_;
  // Scratch for the FILE, OPENED and CHUNK frames. One request runs at a time
  // on the storage worker and respond() copies the frame before returning, so
  // a single buffer lives with the session instead of on the worker stack.
  std::array<std::uint8_t, kPayloadCapacity> io_{};
  ArchiveTransport transport_;
  std::array<OpenFile, kReadHandles> open_{};
  std::uint16_t next_handle_ = 1;
  ble::Link request_link_ = ble::Link::Ble;
  std::uint8_t request_peer_ = 0;
  std::uint32_t request_generation_ = 0;
  const ble::ControlRequest *request_ = nullptr;

  std::size_t slot() const;
  OpenFile &file();
  void release(OpenFile &opened);
  FILE *open_input(const char *path, std::size_t slot, int &failure);
  void close_current();
  std::size_t payload_max() const;
  bool respond(const std::uint8_t *frame, std::size_t length);
  bool error(ble::Op op, ble::Error code, const char *detail = nullptr);
  bool handle_matches(std::uint16_t file_handle) const;
  // An ABORT arrived on this request's link after it (abort_signal.h). The
  // op then ends with ERROR aborted and the marker's ABORTED names it.
  bool aborted() const;
  void stop_aborted(ble::Op op);
  static void emit_file(void *context, const char *name, std::uint32_t bytes);
  void list();
  void open(const std::uint8_t *name_bytes, std::size_t name_length);
  void read(std::uint16_t file_handle, std::uint32_t offset,
            std::uint32_t length);
  void close(std::uint16_t file_handle);
};
} // namespace aqsync
