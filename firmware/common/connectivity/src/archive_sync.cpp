#include "archive_sync.h"
#include <sync_codec.h>

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

std::size_t ArchiveSession::payload_max() const {
  const auto raw = transport_.payload_max(transport_.context, request_link_);
  if (request_link_ == ble::Link::Ble && raw > ble::kMaxFrame)
    return ble::kMaxFrame;
  return raw > kPayloadCapacity ? kPayloadCapacity : raw;
}

bool ArchiveSession::respond(const std::uint8_t *frame, std::size_t length) {
  // Progress during a long LIST/READ must refresh the worker heartbeat.
  if (transport_.progress)
    transport_.progress(transport_.context);
  return transport_.respond(transport_.context, request_link_, frame, length);
}

bool ArchiveSession::error(ble::Op op, ble::Error code, const char *detail) {
  return transport_.error(transport_.context, request_link_, op, code, detail);
}

bool ArchiveSession::handle_matches(std::uint16_t file_handle) const {
  return open_.file && file_handle == open_.handle &&
         request_link_ == open_.link;
}

void ArchiveSession::close() {
  if (open_.file) {
    ArchiveLock lock(archive_);
    std::fclose(open_.file);
  }
  open_ = OpenFile{};
}

void ArchiveSession::reconcile() {
  if (open_.file &&
      open_.generation != transport_.generation(transport_.context, open_.link))
    close();
}

void ArchiveSession::emit_file(void *context, const char *name,
                               std::uint32_t bytes) {
  auto &list = *static_cast<ListContext *>(context);
  if (!list.ok)
    return;
  auto &session = *list.session;
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
    return;
  }
  std::uint8_t frame[5 + 400];
  frame[0] = ble::kFrameFile;
  put_u32(frame + 1, bytes);
  std::memcpy(frame + 5, name, name_length);
  if (!session.respond(frame, 5 + name_length)) {
    if (session.archive_.log) {
      char line[96];
      std::snprintf(line, sizeof(line),
                    "BLE LIST ABORT after=%u reason=notify-refused\n",
                    unsigned(list.count));
      session.archive_.log(session.archive_.context, line);
    }
    list.ok = false;
    return;
  }
  ++list.count;
}

void ArchiveSession::list() {
  ListContext list{this};
  const unsigned partials = archive_.list(archive_.context, emit_file, &list);
  if (!list.ok)
    return;
  std::uint8_t frame[13];
  frame[0] = ble::kFrameListEnd;
  put_u16(frame + 1, list.count);
  put_u16(frame + 3,
          static_cast<std::uint16_t>(partials > 65535 ? 65535 : partials));
  put_u32(frame + 5, archive_.total_kib(archive_.context));
  put_u32(frame + 9, archive_.used_kib(archive_.context));
  respond(frame, sizeof(frame));
  // Preserve the existing LIST semantics: finishing enumeration clears the
  // new-files hint, even if the final LIST_END notification is refused.
  if (archive_.listed)
    archive_.listed(archive_.context);
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
  close();
  std::uint32_t size = 0, crc = 0;
  if (!archive_.finalized(archive_.context, path, size, crc)) {
    error(ble::kOpOpen, ble::kErrNotFinalized, name);
    return;
  }
  FILE *file;
  {
    ArchiveLock lock(archive_);
    file = std::fopen(path, "rb");
  }
  if (!file) {
    error(ble::kOpOpen, ble::kErrOpenFailed, name);
    return;
  }
  open_.file = file;
  open_.handle = next_handle_++;
  if (next_handle_ == 0)
    next_handle_ = 1;
  open_.size = size;
  open_.crc = crc;
  open_.link = request_link_;
  open_.generation = transport_.generation(transport_.context, open_.link);
  std::uint8_t frame[11 + 400];
  frame[0] = ble::kFrameOpened;
  put_u16(frame + 1, open_.handle);
  put_u32(frame + 3, size);
  put_u32(frame + 7, crc);
  std::memcpy(frame + 11, name, name_length);
  if (archive_.log) {
    char line[512];
    std::snprintf(line, sizeof(line),
                  "BLE OPEN handle=%u bytes=%lu crc32=%08lx name=%s\n",
                  unsigned(open_.handle), static_cast<unsigned long>(size),
                  static_cast<unsigned long>(crc), name);
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
  if (offset > open_.size) {
    error(ble::kOpRead, ble::kErrRange);
    return;
  }
  if (length > ble::kMaxRead)
    length = ble::kMaxRead;
  if (offset + length > open_.size)
    length = open_.size - offset;
  const std::size_t max_payload = payload_max();
  if (max_payload <= 7) {
    end(offset, ble::kErrBusy);
    return;
  }
  constexpr std::size_t kChunkCap = kPayloadCapacity - 7;
  const std::size_t chunk_max =
      max_payload - 7 > kChunkCap ? kChunkCap : max_payload - 7;
  bool seek_ok;
  {
    ArchiveLock lock(archive_);
    seek_ok = std::fseek(open_.file, static_cast<long>(offset), SEEK_SET) == 0;
  }
  if (!seek_ok) {
    end(offset, ble::kErrOpenFailed);
    return;
  }
  std::uint8_t frame[7 + kChunkCap];
  std::uint32_t sent = 0;
  while (sent < length) {
    const std::size_t want =
        length - sent < chunk_max ? std::size_t(length - sent) : chunk_max;
    std::size_t count;
    {
      ArchiveLock lock(archive_);
      count = std::fread(frame + 7, 1, want, open_.file);
    }
    if (count == 0) {
      end(offset + sent, ble::kErrOpenFailed);
      return;
    }
    frame[0] = ble::kFrameChunk;
    put_u16(frame + 1, file_handle);
    put_u32(frame + 3, offset + sent);
    if (!respond(frame, 7 + count)) {
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
  close();
  std::uint8_t frame[3];
  frame[0] = ble::kFrameClosed;
  put_u16(frame + 1, file_handle);
  respond(frame, sizeof(frame));
}

bool ArchiveSession::handle(const ble::ControlRequest &request,
                            bool storage_ready) {
  if (!request.length)
    return false;
  const auto op = static_cast<ble::Op>(request.bytes[0]);
  if (op != ble::kOpList && op != ble::kOpOpen && op != ble::kOpRead &&
      op != ble::kOpClose)
    return false;
  request_link_ = request.link;
  reconcile();
  const std::uint8_t *body = request.bytes + 1;
  const std::size_t length = request.length - 1;
  switch (op) {
  case ble::kOpList:
    if (!storage_ready)
      error(op, ble::kErrStorage);
    else
      list();
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
  return true;
}
} // namespace aqsync
