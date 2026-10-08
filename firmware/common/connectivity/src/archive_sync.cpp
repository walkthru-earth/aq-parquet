#include "archive_sync.h"
#include <sync_codec.h>

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

ArchiveSession::OpenFile &ArchiveSession::file() {
  return open_[request_link_ == ble::Link::Lan ? 1 + request_peer_ : 0];
}

bool ArchiveSession::handle_matches(std::uint16_t file_handle) const {
  const auto &opened =
      open_[request_link_ == ble::Link::Lan ? 1 + request_peer_ : 0];
  return opened.handle && file_handle == opened.handle &&
         request_generation_ == opened.generation;
}

void ArchiveSession::close_current() { file() = OpenFile{}; }

void ArchiveSession::close() { open_.fill(OpenFile{}); }

void ArchiveSession::reconcile() {
  for (std::size_t i = 0; i < open_.size(); ++i) {
    auto &opened = open_[i];
    const auto link = i == 0 ? ble::Link::Ble : ble::Link::Lan;
    const auto peer = static_cast<std::uint8_t>(i == 0 ? 0 : i - 1);
    if (opened.handle &&
        opened.generation !=
            transport_.generation(transport_.context, link, peer))
      opened = OpenFile{};
  }
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
    // An incomplete listing must not look like an empty archive to a client.
    list.ok = false;
    list.unsendable = true;
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
  if (!list.ok) {
    if (list.unsendable)
      error(ble::kOpList, ble::kErrBusy, payload_max() >= 6 ? "mtu" : nullptr);
    return;
  }
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
  if (name_length + 11 > payload_max()) {
    error(ble::kOpOpen, ble::kErrBusy, payload_max() >= 6 ? "mtu" : nullptr);
    return;
  }
  close_current();
  std::uint32_t size = 0, crc = 0;
  if (!archive_.finalized(archive_.context, path, size, crc)) {
    error(ble::kOpOpen, ble::kErrNotFinalized, name);
    return;
  }
  bool accessible;
  {
    ArchiveLock lock(archive_);
    FILE *probe = std::fopen(path, "rb");
    accessible = probe != nullptr;
    if (probe)
      std::fclose(probe);
  }
  if (!accessible) {
    error(ble::kOpOpen, ble::kErrOpenFailed, name);
    return;
  }
  auto &opened = this->file();
  std::memcpy(opened.path, path, std::strlen(path) + 1);
  opened.handle = next_handle_++;
  if (next_handle_ == 0)
    next_handle_ = 1;
  opened.size = size;
  opened.crc = crc;
  opened.generation = request_generation_;
  std::uint8_t frame[11 + 400];
  frame[0] = ble::kFrameOpened;
  put_u16(frame + 1, opened.handle);
  put_u32(frame + 3, size);
  put_u32(frame + 7, crc);
  std::memcpy(frame + 11, name, name_length);
  if (archive_.log) {
    char line[512];
    std::snprintf(line, sizeof(line),
                  "BLE OPEN handle=%u bytes=%lu crc32=%08lx name=%s\n",
                  unsigned(opened.handle), static_cast<unsigned long>(size),
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
  const auto &opened = file();
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
  // Only the storage worker accesses files. A transient descriptor keeps the
  // mounted filesystem's existing descriptor budget independent of peers.
  struct ReadFile {
    const ArchiveHooks &archive;
    FILE *value = nullptr;
    explicit ReadFile(const ArchiveHooks &hooks) : archive(hooks) {}
    ReadFile(const ReadFile &) = delete;
    ReadFile &operator=(const ReadFile &) = delete;
    ~ReadFile() {
      if (value) {
        ArchiveLock lock(archive);
        std::fclose(value);
      }
    }
  } input{archive_};
  bool seek_ok;
  {
    ArchiveLock lock(archive_);
    input.value = std::fopen(opened.path, "rb");
    seek_ok = input.value &&
              std::fseek(input.value, static_cast<long>(offset), SEEK_SET) == 0;
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
      count = std::fread(frame + 7, 1, want, input.value);
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
  if (op != ble::kOpList && op != ble::kOpOpen && op != ble::kOpRead &&
      op != ble::kOpClose)
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
