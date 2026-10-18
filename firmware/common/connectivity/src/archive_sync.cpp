#include "archive_sync.h"
#include "abort_signal.h"
#include <sync_codec.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace aqsync {
namespace {
using namespace aq::sync;
class ArchiveLock {
public:
  explicit ArchiveLock(const ArchiveHooks &hooks) : hooks_(hooks) {
    if (hooks_.lock)
      hooks_.lock(hooks_.context);
  }
  ~ArchiveLock() {
    if (hooks_.unlock)
      hooks_.unlock(hooks_.context);
  }

private:
  const ArchiveHooks &hooks_;
};
} // namespace

ArchiveSession::ArchiveSession(const ArchiveHooks &archive,
                               const ArchiveTransport &transport)
    : archive_(archive), transport_(transport) {}

ArchiveSession::~ArchiveSession() {
  cancel_list(false);
  release_files();
}

// Points the reply helpers and the ABORT check at one request or job.
void ArchiveSession::serve(const abort::Tag &tag) {
  request_link_ = tag.link;
  request_peer_ = tag.peer;
  request_generation_ = tag.generation;
  request_tag_ = tag;
  request_ = &request_tag_;
}

std::size_t ArchiveSession::payload_max() const {
  const auto raw =
      transport_.payload_max(transport_.context, request_link_, request_peer_);
  if (request_link_ == ble::Link::Ble && raw > ble::kMaxFrame)
    return ble::kMaxFrame;
  return raw > kPayloadCapacity ? kPayloadCapacity : raw;
}

bool ArchiveSession::respond(const std::uint8_t *frame, std::size_t length) {
  // Progress during a long LIST/READ must refresh the worker heartbeat.
  if (transport_.progress)
    transport_.progress(transport_.context);
  if (request_generation_ !=
      transport_.generation(transport_.context, request_link_, request_peer_))
    return false;
  return transport_.respond(transport_.context, request_link_, request_peer_,
                            request_generation_, frame, length);
}

bool ArchiveSession::error(ble::Op op, ble::Error code, const char *detail) {
  if (request_generation_ !=
      transport_.generation(transport_.context, request_link_, request_peer_))
    return false;
  return transport_.error(transport_.context, request_link_, request_peer_,
                          request_generation_, op, code, detail);
}

bool ArchiveSession::aborted() const {
  return request_ && abort::pending(*request_);
}

void ArchiveSession::stop_aborted(ble::Op op) {
  error(op, ble::kErrAborted);
  abort::note_stopped(*request_, op);
}

std::size_t ArchiveSession::slot() const {
  return request_link_ == ble::Link::Lan ? 1 + request_peer_ : 0;
}

ArchiveSession::OpenFile &ArchiveSession::file() { return open_[slot()]; }

bool ArchiveSession::handle_matches(std::uint16_t file_handle) const {
  const auto &opened = open_[slot()];
  return opened.handle && file_handle == opened.handle &&
         request_generation_ == opened.generation;
}

void ArchiveSession::release(OpenFile &opened) {
  if (!opened.file)
    return;
  ArchiveLock lock(archive_);
  std::fclose(opened.file);
  opened.file = nullptr;
}

FILE *ArchiveSession::open_input(const char *path, std::size_t index,
                                 int &failure) {
  ArchiveLock lock(archive_);
  errno = 0;
  FILE *input = std::fopen(path, "rb");
  failure = input ? 0 : errno;
  // A slot buffer turns each READ window into multi-sector reads.
  void *buffer = input && archive_.read_buffer
                     ? archive_.read_buffer(archive_.context, index)
                     : nullptr;
  if (buffer)
    std::setvbuf(input, static_cast<char *>(buffer), _IOFBF, kReadBufferBytes);
  return input;
}

void ArchiveSession::close_current() {
  release(file());
  file() = OpenFile{};
}

void ArchiveSession::close() {
  cancel_list(true);
  for (auto &opened : open_)
    release(opened);
  open_.fill(OpenFile{});
}

void ArchiveSession::release_files() {
  for (auto &opened : open_)
    release(opened);
}

void ArchiveSession::remounted() {
  // A remount is transient, so the LIST ends busy and the client retries.
  cancel_list(true, ble::kErrBusy, "remount");
  mounted_ = false;
  for (auto &opened : open_) {
    release(opened);
    if (opened.handle)
      opened.verify = true;
  }
}

// Runs on the stream read() just reopened, still at offset 0. A handle names
// the bytes OPENED described, never whatever now sits at its path. Only a
// completed check proves a change. An I/O error keeps the recheck pending.
ArchiveSession::Recheck ArchiveSession::reverify(OpenFile &opened) {
  const std::int64_t started =
      archive_.now_us ? archive_.now_us(archive_.context) : 0;
  FileCheck check;
  const bool finalized =
      archive_.finalized(archive_.context, opened.path, opened.file, check);
  Recheck result = Recheck::Same;
  if (!finalized)
    result = check.io_error ? Recheck::Unreadable : Recheck::Changed;
  else if (check.size != opened.size || check.crc != opened.crc)
    result = Recheck::Changed;
  if (archive_.log) {
    const std::int64_t elapsed =
        archive_.now_us ? archive_.now_us(archive_.context) - started : -1;
    char line[112];
    std::snprintf(line, sizeof(line),
                  "BLE READ RECHECK handle=%u result=%s check_us=%lld\n",
                  unsigned(opened.handle),
                  result == Recheck::Same      ? "same"
                  : result == Recheck::Changed ? "changed"
                                               : "io_error",
                  static_cast<long long>(elapsed));
    archive_.log(archive_.context, line);
  }
  if (result == Recheck::Same)
    opened.verify = false;
  return result;
}

void ArchiveSession::reconcile() {
  for (std::size_t i = 0; i < open_.size(); ++i) {
    auto &opened = open_[i];
    const auto link = i == 0 ? ble::Link::Ble : ble::Link::Lan;
    const auto peer = static_cast<std::uint8_t>(i == 0 ? 0 : i - 1);
    if (opened.handle &&
        opened.generation !=
            transport_.generation(transport_.context, link, peer)) {
      release(opened);
      opened = OpenFile{};
    }
  }
}

bool ArchiveSession::emit_file(void *context, const char *name,
                               std::uint32_t bytes) {
  auto &session = *static_cast<ArchiveSession *>(context);
  auto &list = session.list_;
  if (!list.ok || session.stop_list())
    return false;
  if (list.count == UINT32_MAX) {
    list.ok = false;
    list.failure_detail = "count";
    return false;
  }
  const std::size_t name_length = std::strlen(name);
  const std::size_t payload_max = session.payload_max();
  if (payload_max < 6 || name_length + 5 > payload_max || name_length > 400) {
    if (session.archive_.log) {
      char line[96];
      std::snprintf(line, sizeof(line),
                    "BLE LIST SKIP name_bytes=%u payload_max=%u\n",
                    unsigned(name_length), unsigned(payload_max));
      session.archive_.log(session.archive_.context, line);
    }
    // An incomplete listing must not look like an empty archive to a client.
    list.ok = false;
    list.failure_detail = "mtu";
    return false;
  }
  static_assert(5 + 400 <= kPayloadCapacity, "FILE frame exceeds scratch");
  std::uint8_t *frame = session.io_.data();
  frame[0] = ble::kFrameFile;
  put_u32(frame + 1, bytes);
  std::memcpy(frame + 5, name, name_length);
  if (!session.respond(frame, 5 + name_length)) {
    // The transport may refuse the frame for the same reason the predicate
    // names, an ABORT or a preemption seen while it waited for room. The
    // reply then says which. Anything else is a refused frame.
    if (!session.stop_list())
      list.ok = false;
    return false;
  }
  ++list.count;
  return true;
}

ArchiveSession::Stop ArchiveSession::should_stop() const {
  if (aborted())
    return Stop::Aborted;
  if (archive_.preempted && archive_.preempted(archive_.context))
    return Stop::Preempted;
  return Stop::None;
}

bool ArchiveSession::stop_list() {
  switch (should_stop()) {
  case Stop::None:
    return false;
  case Stop::Aborted:
    list_.aborted = true;
    break;
  case Stop::Preempted:
    list_.failure_detail = "preempted";
    break;
  }
  list_.ok = false;
  return true;
}

bool ArchiveSession::list_connected() const {
  return list_.tag.generation == transport_.generation(transport_.context,
                                                       list_.tag.link,
                                                       list_.tag.peer);
}

bool ArchiveSession::must_wait_for_list(
    const ble::ControlRequest &request) const {
  if (!list_.active || !request.length)
    return false;
  return request.link == list_.tag.link && request.peer == list_.tag.peer;
}

// Ends the job outside step(), for an erase, a remount or a preemption. The
// request being served, if any, keeps its reply route.
void ArchiveSession::cancel_list(bool notify, ble::Error code,
                                 const char *detail) {
  if (!list_.active)
    return;
  if (list_.begun && archive_.list_cancel)
    archive_.list_cancel(archive_.context);
  list_.active = false;
  if (!notify || !list_connected())
    return;
  const auto link = request_link_;
  const auto peer = request_peer_;
  const auto generation = request_generation_;
  const auto *request = request_;
  const auto tag = request_tag_;
  serve(list_.tag);
  if (archive_.log) {
    char line[96];
    std::snprintf(
        line, sizeof(line), "LIST ABORT link=%s after=%lu reason=%s\n",
        request_link_ == ble::Link::Lan ? "lan" : "ble",
        static_cast<unsigned long>(list_.count), detail ? detail : "storage");
    archive_.log(archive_.context, line);
  }
  error(list_.op, code,
        detail && payload_max() >= 3 + std::strlen(detail) ? detail : nullptr);
  request_link_ = link;
  request_peer_ = peer;
  request_generation_ = generation;
  request_tag_ = tag;
  request_ = request ? &request_tag_ : nullptr;
}

void ArchiveSession::preempt_list() {
  if (!list_.active)
    return;
  // Called between requests, but keep any reply route like cancel_list().
  const auto link = request_link_;
  const auto peer = request_peer_;
  const auto generation = request_generation_;
  const auto *request = request_;
  const auto tag = request_tag_;
  serve(list_.tag);
  if (!list_connected()) {
    cancel_list(false);
  } else {
    // A pending ABORT still wins, as in should_stop().
    if (aborted())
      list_.aborted = true;
    else
      list_.failure_detail = "preempted";
    list_.ok = false;
    finish_list(ListStatus::Stopped);
  }
  request_link_ = link;
  request_peer_ = peer;
  request_generation_ = generation;
  request_tag_ = tag;
  request_ = request ? &request_tag_ : nullptr;
}

void ArchiveSession::step(bool storage_ready) {
  if (!list_.active)
    return;
  serve(list_.tag);
  // A peer that left, or was replaced, gets no frames. Its walk just ends.
  if (!list_connected()) {
    cancel_list(false);
    request_ = nullptr;
    return;
  }
  ListStatus status;
  if (stop_list()) {
    // Checked before any directory read, so ABORT or a preemption stops the
    // walk within one step even when no entry of the last step was a
    // finalized file.
    status = ListStatus::Stopped;
  } else if (!storage_ready) {
    status = ListStatus::Failed;
  } else {
    const bool begin = !list_.begun;
    list_.begun = true;
    if (list_.op == ble::kOpListSince)
      status = archive_.list_since(archive_.context, begin, list_.cursor,
                                   list_.since, kListStepEntries, emit_file,
                                   this, list_.partials);
    else
      status = archive_.list(archive_.context, begin, kListStepEntries,
                             emit_file, this, list_.partials);
  }
  if (status != ListStatus::More)
    finish_list(status);
  request_ = nullptr;
}

void ArchiveSession::finish_list(ListStatus status) {
  // The hook closes its walk itself unless this job ended it early. The
  // cancel hook is safe either way.
  if (list_.begun && archive_.list_cancel)
    archive_.list_cancel(archive_.context);
  list_.active = false;
  const auto &list = list_;
  if (!list.ok && archive_.log) {
    char line[96];
    std::snprintf(line, sizeof(line),
                  "LIST ABORT link=%s after=%lu reason=%s\n",
                  request_link_ == ble::Link::Lan ? "lan" : "ble",
                  static_cast<unsigned long>(list.count),
                  list.aborted          ? "aborted"
                  : list.failure_detail ? list.failure_detail
                                        : "refused");
    archive_.log(archive_.context, line);
  }
  // An ABORT wins over a walk error, since the client asked to stop.
  if (list.aborted) {
    stop_aborted(list.op);
    return;
  }
  if (status == ListStatus::Failed) {
    error(list.op, ble::kErrStorage);
    return;
  }
  if (!list.ok || status == ListStatus::Stopped) {
    // Always try a terminal ERROR so the client never waits for LIST_END.
    const char *detail = list.failure_detail ? list.failure_detail : "refused";
    error(list.op, ble::kErrBusy,
          payload_max() >= 3 + std::strlen(detail) ? detail : nullptr);
    return;
  }
  const bool since = list.op == ble::kOpListSince;
  // LIST_SINCE_END needs 48 bytes. A link that shrank below that since the
  // job started gets the LIST reply for a frame that does not fit.
  if (since && payload_max() < kListSinceEndBytes) {
    error(list.op, ble::kErrBusy, "mtu");
    return;
  }
  // LIST_END is the first 15 bytes of LIST_SINCE_END.
  std::uint8_t frame[kListSinceEndBytes];
  frame[0] = since ? ble::kFrameListSinceEnd : ble::kFrameListEnd;
  put_u32(frame + 1, list.count);
  put_u16(frame + 5, static_cast<std::uint16_t>(
                         list.partials > 65535 ? 65535 : list.partials));
  put_u32(frame + 7, archive_.total_kib(archive_.context));
  put_u32(frame + 11, archive_.used_kib(archive_.context));
  std::size_t length = 15;
  if (since) {
    frame[15] = list.since.flags;
    encode_cursor(list.since.next, std::span<std::uint8_t, kListCursorBytes>(
                                       frame + 16, kListCursorBytes));
    length = kListSinceEndBytes;
  }
  // A failed terminal delivery leaves the client without a complete snapshot.
  if (respond(frame, length) && archive_.listed)
    archive_.listed(archive_.context);
}

void ArchiveSession::start_list(ble::Op op, const ble::ControlRequest &request,
                                bool storage_ready) {
  ListCursor cursor;
  if (op == ble::kOpListSince) {
    if (request.length != kListSinceRequestBytes) {
      error(op, ble::kErrMalformed, "cursor");
      return;
    }
    cursor = decode_cursor(std::span<const std::uint8_t, kListCursorBytes>(
        request.bytes + 1, kListCursorBytes));
    if (cursor.day != 0 && !valid_list_day(cursor.day)) {
      error(op, ble::kErrMalformed, "cursor");
      return;
    }
  }
  if (!storage_ready) {
    error(op, ble::kErrStorage);
  } else if (list_.active) {
    // One LIST runs at a time. Only the job's own peer waits for it.
    error(op, ble::kErrBusy, payload_max() >= 7 ? "list" : nullptr);
  } else if (archive_.preempted && archive_.preempted(archive_.context)) {
    // A close is already due, so the job would end on its first step.
    error(op, ble::kErrBusy, payload_max() >= 12 ? "preempted" : nullptr);
  } else if (op == ble::kOpListSince && payload_max() < kListSinceEndBytes) {
    // LIST_SINCE_END could never be delivered.
    error(op, ble::kErrBusy, "mtu");
  } else {
    // step() walks it, one bounded step per worker pass.
    list_ = ListJob{};
    list_.op = op;
    list_.active = true;
    list_.tag = abort::tag(request);
    list_.cursor = cursor;
  }
}

void ArchiveSession::open(const std::uint8_t *name_bytes,
                          std::size_t name_length) {
  char name[400];
  if (name_length == 0 || name_length >= sizeof(name)) {
    error(ble::kOpOpen, ble::kErrInvalidName, "length");
    return;
  }
  std::memcpy(name, name_bytes, name_length);
  name[name_length] = '\0';
  char path[416];
  if (!archive_.resolve(archive_.context, name, path, sizeof(path))) {
    error(ble::kOpOpen, ble::kErrInvalidName, "charset");
    return;
  }
  if (name_length + 11 > payload_max()) {
    error(ble::kOpOpen, ble::kErrBusy, payload_max() >= 6 ? "mtu" : nullptr);
    return;
  }
  close_current();
  const std::int64_t started =
      archive_.now_us ? archive_.now_us(archive_.context) : 0;
  // The descriptor opened here serves the check and every later READ.
  int failure = 0;
  FILE *input = open_input(path, slot(), failure);
  if (!input) {
    if (archive_.log) {
      char line[64];
      std::snprintf(line, sizeof(line), "BLE OPEN FAIL errno=%d\n", failure);
      archive_.log(archive_.context, line);
    }
    // A missing path is not a finalized file. Other failures, such as a full
    // descriptor table, are open failures.
    error(ble::kOpOpen,
          failure == ENOENT ? ble::kErrNotFinalized : ble::kErrOpenFailed,
          name);
    return;
  }
  FileCheck check;
  if (!archive_.finalized(archive_.context, path, input, check)) {
    {
      ArchiveLock lock(archive_);
      std::fclose(input);
    }
    error(ble::kOpOpen, ble::kErrNotFinalized, name);
    return;
  }
  const std::uint32_t size = check.size, crc = check.crc;
  auto &opened = this->file();
  opened.file = input;
  std::memcpy(opened.path, path, std::strlen(path) + 1);
  opened.handle = next_handle_++;
  if (next_handle_ == 0)
    next_handle_ = 1;
  opened.size = size;
  opened.crc = crc;
  opened.generation = request_generation_;
  static_assert(11 + 400 <= kPayloadCapacity, "OPENED frame exceeds scratch");
  std::uint8_t *frame = io_.data();
  frame[0] = ble::kFrameOpened;
  put_u16(frame + 1, opened.handle);
  put_u32(frame + 3, size);
  put_u32(frame + 7, crc);
  std::memcpy(frame + 11, name, name_length);
  if (archive_.log) {
    const std::int64_t elapsed =
        archive_.now_us ? archive_.now_us(archive_.context) - started : -1;
    char line[560];
    std::snprintf(
        line, sizeof(line),
        "BLE OPEN handle=%u bytes=%lu crc32=%08lx crc=%s open_us=%lld "
        "name=%s\n",
        unsigned(opened.handle), static_cast<unsigned long>(size),
        static_cast<unsigned long>(crc), check.cached ? "hit" : "miss",
        static_cast<long long>(elapsed), name);
    archive_.log(archive_.context, line);
  }
  respond(frame, 11 + name_length);
}

void ArchiveSession::read(std::uint16_t file_handle, std::uint32_t offset,
                          std::uint32_t length) {
  auto end = [&](std::uint32_t next, ble::Error status) {
    std::uint8_t frame[8];
    frame[0] = ble::kFrameReadEnd;
    put_u16(frame + 1, file_handle);
    put_u32(frame + 3, next);
    frame[7] = static_cast<std::uint8_t>(status);
    respond(frame, sizeof(frame));
  };
  if (!handle_matches(file_handle)) {
    error(ble::kOpRead, ble::kErrBadHandle);
    return;
  }
  auto &opened = file();
  if (offset > opened.size) {
    error(ble::kOpRead, ble::kErrRange);
    return;
  }
  if (length > ble::kMaxRead)
    length = ble::kMaxRead;
  if (length > opened.size - offset)
    length = opened.size - offset;
  const std::size_t max_payload = payload_max();
  if (max_payload <= 7) {
    end(offset, ble::kErrBusy);
    return;
  }
  constexpr std::size_t kChunkCap = kPayloadCapacity - 7;
  const std::size_t chunk_max =
      max_payload - 7 > kChunkCap ? kChunkCap : max_payload - 7;
  // Only the storage worker accesses files. The handle's descriptor stays
  // open across READ windows. After release_files(), remounted() or a read
  // failure the path is reopened here. After a remount the reopened file must
  // still be the one OPENED described, or the handle is gone. Only proof of a
  // change ends it. An unmounted volume or an I/O error leaves the stream
  // closed, so READ_END status 4 below keeps the recheck pending.
  if (!opened.file && !(opened.verify && !mounted_)) {
    int failure = 0;
    opened.file = open_input(opened.path, slot(), failure);
    const Recheck recheck = !opened.verify      ? Recheck::Same
                            : opened.file       ? reverify(opened)
                            : failure == ENOENT ? Recheck::Changed
                                                : Recheck::Unreadable;
    if (recheck == Recheck::Changed) {
      close_current();
      error(ble::kOpRead, ble::kErrBadHandle);
      return;
    }
    if (recheck == Recheck::Unreadable)
      release(opened);
  }
  bool seek_ok = false;
  if (opened.file) {
    ArchiveLock lock(archive_);
    seek_ok = std::fseek(opened.file, static_cast<long>(offset), SEEK_SET) == 0;
  }
  if (!seek_ok) {
    release(opened);
    end(offset, ble::kErrOpenFailed);
    return;
  }
  static_assert(7 + kChunkCap <= kPayloadCapacity, "CHUNK exceeds scratch");
  std::uint8_t *frame = io_.data();
  std::uint32_t sent = 0;
  while (sent < length) {
    // An aborted READ ends with ERROR aborted and no READ_END.
    if (aborted()) {
      stop_aborted(ble::kOpRead);
      return;
    }
    const std::size_t want =
        length - sent < chunk_max ? std::size_t(length - sent) : chunk_max;
    std::size_t count;
    {
      ArchiveLock lock(archive_);
      count = std::fread(frame + 7, 1, want, opened.file);
    }
    if (count == 0) {
      // Drop the stream so a retry starts from a fresh descriptor.
      release(opened);
      end(offset + sent, ble::kErrOpenFailed);
      return;
    }
    frame[0] = ble::kFrameChunk;
    put_u16(frame + 1, file_handle);
    put_u32(frame + 3, offset + sent);
    if (!respond(frame, 7 + count)) {
      if (aborted())
        stop_aborted(ble::kOpRead);
      else
        end(offset + sent, ble::kErrBusy);
      return;
    }
    sent += count;
  }
  if (archive_.log) {
    char line[128];
    std::snprintf(line, sizeof(line),
                  "BLE READ file_handle=%u offset=%lu bytes=%lu\n",
                  unsigned(file_handle), static_cast<unsigned long>(offset),
                  static_cast<unsigned long>(sent));
    archive_.log(archive_.context, line);
  }
  end(offset + sent, static_cast<ble::Error>(0));
}

void ArchiveSession::close(std::uint16_t file_handle) {
  if (!handle_matches(file_handle)) {
    error(ble::kOpClose, ble::kErrBadHandle);
    return;
  }
  close_current();
  std::uint8_t frame[3];
  frame[0] = ble::kFrameClosed;
  put_u16(frame + 1, file_handle);
  respond(frame, sizeof(frame));
}

bool ArchiveSession::handle(const ble::ControlRequest &request,
                            bool storage_ready) {
  if (!request.length)
    return false;
  if (request.length > sizeof(request.bytes))
    return true;
  const auto op = static_cast<ble::Op>(request.bytes[0]);
  if (op != ble::kOpList && op != ble::kOpListSince && op != ble::kOpOpen &&
      op != ble::kOpRead && op != ble::kOpClose)
    return false;
  reconcile();
  if ((request.link == ble::Link::Lan && request.peer >= ble::kMaxLanClients) ||
      (request.link == ble::Link::Ble && request.peer != 0))
    return true;
  request_link_ = request.link;
  request_peer_ = request.peer;
  request_generation_ = request.link_generation;
  // Queued commands from a disconnected peer must not change a replacement
  // peer's handle or deliver frames to that peer.
  if (request_generation_ !=
      transport_.generation(transport_.context, request_link_, request_peer_))
    return true;
  // Set only here, so every later return clears it and no stale stamp
  // outlives the request.
  serve(abort::tag(request));
  const std::uint8_t *body = request.bytes + 1;
  const std::size_t length = request.length - 1;
  // Without the hook the op does not exist on this board. The client then
  // falls back to LIST.
  if (op == ble::kOpListSince && !archive_.list_since) {
    error(op, ble::kErrUnknownOp);
    request_ = nullptr;
    return true;
  }
  // A LIST or READ queued before an ABORT on its link never starts.
  if ((op == ble::kOpList || op == ble::kOpListSince || op == ble::kOpRead) &&
      aborted()) {
    stop_aborted(op);
    request_ = nullptr;
    return true;
  }
  switch (op) {
  case ble::kOpList:
  case ble::kOpListSince:
    start_list(op, request, storage_ready);
    break;
  case ble::kOpOpen:
    if (!storage_ready)
      error(op, ble::kErrStorage);
    else
      open(body, length);
    break;
  case ble::kOpRead:
    if (length != 10)
      error(op, ble::kErrMalformed);
    else
      read(get_u16(body), get_u32(body + 2), get_u32(body + 6));
    break;
  case ble::kOpClose:
    if (length != 2)
      error(op, ble::kErrMalformed);
    else
      close(get_u16(body));
    break;
  default:
    break;
  }
  request_ = nullptr;
  return true;
}
} // namespace aqsync
