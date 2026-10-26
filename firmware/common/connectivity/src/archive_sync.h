#pragma once

#include "abort_signal.h"
#include "ble_sync.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>

namespace ossync {
// Receives one finalized file of a listing. Returning false stops the walk at
// once, for a refused frame or a pending ABORT.
using FileEmitter = bool (*)(void *context, const char *name,
                             std::uint32_t bytes);

// Result of the finalized check behind OPEN. cached is true when the CRC came
// from the owner's finalize-time cache instead of a full read. io_error is
// set when a failed check stopped at a filesystem call or a short read, so the
// false result says nothing about the file itself.
struct FileCheck {
  std::uint32_t size = 0;
  std::uint32_t crc = 0;
  bool cached = false;
  bool io_error = false;
};

// One step of a resumable LIST walk. More: entries remain and the walk holds
// its directories open until the next step or list_cancel. Complete: every
// configured archive tree was read. Failed: a filesystem call failed, so the
// FILE frames already emitted are an incomplete snapshot. Stopped: the
// emitter returned false. Only More leaves anything open.
enum class ListStatus : std::uint8_t { More, Complete, Failed, Stopped };

// LIST_SINCE cursor (docs/shared/ble-sync-protocol.md "LIST_SINCE"). The
// request carries it after the opcode and LIST_SINCE_END returns the next one.
// day is YYYYMMDD UTC, 0 for no floor. boot is the 16 raw bytes of the 32 hex
// boot id in text order, all zero for no unsynced position. seq is the lowest
// first sequence of that boot still to list. mount_epoch 0 never matches.
struct ListCursor {
  std::uint32_t mount_epoch = 0;
  std::uint32_t day = 0;
  std::array<std::uint8_t, 16> boot{};
  std::uint64_t seq = 0;
};
inline constexpr std::size_t kListCursorBytes = 4 + 4 + 16 + 8;
inline constexpr std::size_t kListSinceRequestBytes = 1 + kListCursorBytes;
inline constexpr std::size_t kListSinceEndBytes = 15 + 1 + kListCursorBytes;
static_assert(kListSinceRequestBytes == 33 && kListSinceEndBytes == 48);
// LIST_SINCE_END flags.
inline constexpr std::uint8_t kListSinceFull = 1U << 0;
inline constexpr std::uint8_t kListSinceUnsyncedFull = 1U << 1;

// A real UTC calendar day from 2020-01-01 to 2100-12-31.
constexpr bool valid_list_day(std::uint32_t day) {
  const std::uint32_t year = day / 10000, month = day / 100 % 100,
                      date = day % 100;
  if (year < 2020 || year > 2100 || month < 1 || month > 12 || date < 1)
    return false;
  constexpr std::uint8_t kDays[] = {31, 28, 31, 30, 31, 30,
                                    31, 31, 30, 31, 30, 31};
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  return date <= kDays[month - 1] + (month == 2 && leap ? 1U : 0U);
}

// Little-endian cursor bytes, the layout of the request after the opcode and
// of LIST_SINCE_END after its flags byte.
constexpr void encode_cursor(const ListCursor &cursor,
                             std::span<std::uint8_t, kListCursorBytes> out) {
  auto put = [&](std::size_t at, std::uint64_t value, std::size_t bytes) {
    for (std::size_t i = 0; i < bytes; ++i)
      out[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
  };
  put(0, cursor.mount_epoch, 4);
  put(4, cursor.day, 4);
  for (std::size_t i = 0; i < cursor.boot.size(); ++i)
    out[8 + i] = cursor.boot[i];
  put(24, cursor.seq, 8);
}
constexpr ListCursor
decode_cursor(std::span<const std::uint8_t, kListCursorBytes> in) {
  auto get = [&](std::size_t at, std::size_t bytes) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < bytes; ++i)
      value |= static_cast<std::uint64_t>(in[at + i]) << (8 * i);
    return value;
  };
  ListCursor cursor;
  cursor.mount_epoch = static_cast<std::uint32_t>(get(0, 4));
  cursor.day = static_cast<std::uint32_t>(get(4, 4));
  for (std::size_t i = 0; i < cursor.boot.size(); ++i)
    cursor.boot[i] = in[8 + i];
  cursor.seq = get(24, 8);
  return cursor;
}

// What a LIST_SINCE walk decided at its start: the flags and the cursor that
// LIST_SINCE_END returns.
struct ListSinceResult {
  std::uint8_t flags = 0;
  ListCursor next;
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
  // Resumable LIST walk. begin is true on the first step of a walk and drops
  // any walk left open. Each call reads at most `budget` directory entries
  // (at least one, so every step makes progress), emits each finalized file
  // and adds each retained .partial to `partials`.
  ListStatus (*list)(void *, bool begin, unsigned budget, FileEmitter, void *,
                     unsigned &partials) = nullptr;
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
  // Closes a walk that list left at More. Must be safe to call when nothing
  // is open. Optional when list holds nothing open between steps.
  void (*list_cancel)(void *) = nullptr;
  // Optional, read only. True while the storage owner needs the worker for
  // something more urgent, for example a critical battery close. A running
  // LIST job then ends with ERROR busy "preempted" before its next directory
  // read or FILE frame, and a new LIST gets the same reply at once. Must not
  // consume the request it reports.
  bool (*preempted)(void *) = nullptr;
  // Optional resumable LIST_SINCE walk with the contract of list. On the
  // begin step it reads `cursor` and fills `result` before any directory
  // read, so the next cursor covers every file finalized after the walk
  // starts. Without it LIST_SINCE answers ERROR unknown op, the client's
  // signal to fall back to LIST.
  ListStatus (*list_since)(void *, bool begin, const ListCursor &cursor,
                           ListSinceResult &result, unsigned budget,
                           FileEmitter, void *, unsigned &partials) = nullptr;
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

// LIST runs as a job of bounded steps. handle() only starts it. The storage
// worker then calls step() once per loop pass, after it took samples and
// committed, so a large card never holds the worker for a whole walk. Each
// step reads at most kListStepEntries directory entries and checks ABORT
// first, so an ABORT stops the walk within one step. One LIST runs at a
// time, and a LIST from another link or peer gets ERROR busy "list". While
// the job runs the worker holds back every request that must_wait_for_list()
// names and serves the others between steps.
//
// Independent immutable logical file handles for BLE and each LAN peer, bound
// to their connection generations. Each open handle keeps one read-only FILE
// until CLOSE, a re-OPEN on its slot, a generation change, close(),
// release_files() or remounted(), so READ windows seek instead of reopening.
// The mount must budget kReadHandles descriptors for these slots
// (archive_files.h). Construct once, then call exclusively from the storage
// worker that owns the archive. Radio callbacks must enqueue ControlRequest
// copies; they must never call this class directly.
class ArchiveSession {
public:
  static constexpr std::size_t kReadHandles = 1 + ble::kMaxLanClients;
  static constexpr std::size_t kReadBufferBytes = 4096;
  // Directory entries one LIST step may read. Each is a readdir and a stat,
  // plus a FILE frame for a finalized file.
  static constexpr unsigned kListStepEntries = 32;

  ArchiveSession(const ArchiveHooks &archive,
                 const ArchiveTransport &transport);
  ~ArchiveSession();
  ArchiveSession(const ArchiveSession &) = delete;
  ArchiveSession &operator=(const ArchiveSession &) = delete;

  // Returns true for LIST/LIST_SINCE/OPEN/READ/CLOSE (including error
  // responses), false for other operations so the board's
  // clock/config/logger handler can run. LIST_SINCE without the list_since
  // hook answers ERROR unknown op.
  bool handle(const ble::ControlRequest &request, bool storage_ready);
  // True while a LIST job is in progress.
  bool listing() const { return list_.active; }
  // Runs one bounded step of the LIST job. The last step sends LIST_END or
  // a terminal ERROR. A job whose connection changed ends without a frame.
  // Storage that is no longer ready ends the job with ERROR storage.
  void step(bool storage_ready);
  // The job's link, peer, generation and ABORT stamp, for the worker's wait
  // hooks while step() runs. nullptr when no LIST job is active.
  const abort::Tag *list_tag() const {
    return list_.active ? &list_.tag : nullptr;
  }
  // True when `request` must wait for the LIST job to end. That is any
  // request on the job's link and peer, ABORT markers included, so replies
  // keep their order and ABORTED follows ERROR aborted. A LIST from another
  // link or peer does not wait. handle() answers it ERROR busy "list".
  bool must_wait_for_list(const ble::ControlRequest &request) const;
  void reconcile();
  // Drops every logical handle and closes its descriptor. Ends a LIST job
  // with ERROR storage.
  void close();
  // Ends a running LIST job with ERROR busy "preempted", or ERROR aborted
  // when an ABORT is already pending, and closes its walk. The worker calls
  // it before a critical battery close so the close never remounts under a
  // live walk. No frame when the job's peer left.
  void preempt_list();
  // Closes descriptors but keeps logical handles. The next READ reopens the
  // path without checking it again, so the card must stay mounted.
  void release_files();
  // Call before a storage remount. Ends a LIST job with ERROR busy
  // "remount", since its open directories cannot outlive the mount and the
  // client should simply retry. Like release_files(), but
  // the card may be edited or swapped meanwhile, so the next READ on each
  // handle runs the finalized check again. Same size and CRC keep the handle. A
  // different file, or a missing or no longer finalized one on the mounted
  // volume, drops it and READ answers ERROR bad handle. Until mounted() is
  // called, or when the check hits an I/O error, READ answers READ_END status 4
  // and the recheck stays pending.
  void remounted();
  // Call after the volume remounted successfully.
  void mounted() { mounted_ = true; }

private:
  struct OpenFile {
    char path[416]{};
    FILE *file = nullptr;
    std::uint16_t handle = 0;
    std::uint32_t size = 0;
    std::uint32_t crc = 0;
    std::uint32_t generation = 0;
    // Set by remounted(). The next reopen must match size and crc.
    bool verify = false;
  };
  enum class Recheck { Same, Changed, Unreadable };
  struct ListJob {
    // kOpList or kOpListSince. Every reply of the job names it.
    ble::Op op = ble::kOpList;
    bool active = false;
    bool begun = false;
    bool ok = true;
    bool aborted = false;
    abort::Tag tag;
    std::uint32_t count = 0;
    unsigned partials = 0;
    const char *failure_detail = nullptr;
    ListCursor cursor;
    ListSinceResult since;
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
  // False from remounted() until mounted(). A pending handle is not rechecked
  // meanwhile, because an absent volume answers every path as missing.
  bool mounted_ = true;
  ble::Link request_link_ = ble::Link::Ble;
  std::uint8_t request_peer_ = 0;
  std::uint32_t request_generation_ = 0;
  // The ABORT stamp of the request or LIST step being served, or nullptr.
  const abort::Tag *request_ = nullptr;
  abort::Tag request_tag_;
  ListJob list_;

  std::size_t slot() const;
  OpenFile &file();
  void release(OpenFile &opened);
  Recheck reverify(OpenFile &opened);
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
  enum class Stop : std::uint8_t { None, Aborted, Preempted };
  // The single stop predicate of a LIST job, checked before every step and
  // every FILE frame. An ABORT wins over a preemption.
  Stop should_stop() const;
  // Records a pending stop on the job. True when the walk must end.
  bool stop_list();
  static bool emit_file(void *context, const char *name, std::uint32_t bytes);
  void serve(const abort::Tag &tag);
  bool list_connected() const;
  void finish_list(ListStatus status);
  void start_list(ble::Op op, const ble::ControlRequest &request,
                  bool storage_ready);
  void cancel_list(bool notify, ble::Error code = ble::kErrStorage,
                   const char *detail = nullptr);
  void open(const std::uint8_t *name_bytes, std::size_t name_length);
  void read(std::uint16_t file_handle, std::uint32_t offset,
            std::uint32_t length);
  void close(std::uint16_t file_handle);
};
} // namespace ossync
