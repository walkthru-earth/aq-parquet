#pragma once

// ABORT (docs/shared/ble-sync-protocol.md, "ABORT and ABORTED"). A transport
// task stamps every request with its link's abort sequence before queueing it,
// and an accepted ABORT advances that sequence. The storage worker then
// sees a request whose stamp is behind as aborted, wherever it is waiting, and
// the ABORT itself reaches the worker later as an ordinary queued marker.
// Sequences are atomics shared by the transport tasks and the worker. The
// stopped op is worker state only.

#include "ble_sync.h"

#include <atomic>
#include <cstdint>

namespace aqsync::abort {
constexpr std::size_t kSlots = 1 + ble::kMaxLanClients;

struct Stopped {
  std::uint32_t generation = 0;
  std::uint8_t op = 0; // first op stopped since the last ABORTED, 0 for none
};

// The parts of a request ABORT needs, so a job that outlives its request can
// keep checking without holding the whole request.
struct Tag {
  ble::Link link = ble::Link::Ble;
  std::uint8_t peer = 0;
  std::uint32_t generation = 0;
  std::uint32_t sequence = 0;
};

inline Tag tag(const ble::ControlRequest &request) {
  return {request.link, request.peer, request.link_generation,
          request.abort_seq};
}

inline std::atomic<std::uint32_t> sequences[kSlots]{};
inline Stopped stopped[kSlots]{};

inline std::size_t slot(ble::Link link, std::uint8_t peer) {
  if (link != ble::Link::Lan)
    return 0;
  return peer < ble::kMaxLanClients ? 1 + peer : 0;
}

// Transport tasks, before handing a request to the queue. An ABORT is stamped
// with the sequence it will advance to. Each slot has one transport writer.
inline void stamp(ble::ControlRequest &request) {
  const std::uint32_t current =
      sequences[slot(request.link, request.peer)].load();
  request.abort_seq = request.length && request.bytes[0] == ble::kOpAbort
                          ? current + 1
                          : current;
}

// Transport tasks, once the queue accepted the request. Only an accepted ABORT
// advances the sequence, so a refused one stops nothing and the stop it causes
// always meets its marker.
inline void commit(const ble::ControlRequest &request) {
  if (request.length && request.bytes[0] == ble::kOpAbort)
    sequences[slot(request.link, request.peer)].store(request.abort_seq);
}

// Worker. True once an ABORT arrived after this request was received.
inline bool pending(const Tag &request) {
  return sequences[slot(request.link, request.peer)].load() != request.sequence;
}
inline bool pending(const ble::ControlRequest &request) {
  return pending(tag(request));
}

// Worker. Remembers the first op an ABORT stopped on this connection; the
// following ABORT marker names it in ABORTED.
inline void note_stopped(const Tag &request, ble::Op op) {
  auto &entry = stopped[slot(request.link, request.peer)];
  if (entry.op && entry.generation == request.generation)
    return;
  entry.generation = request.generation;
  entry.op = static_cast<std::uint8_t>(op);
}
inline void note_stopped(const ble::ControlRequest &request, ble::Op op) {
  note_stopped(tag(request), op);
}

// Worker, for the ABORT marker. Returns the stopped op, or 0 when nothing was
// running, and clears it so every ABORT gets exactly one ABORTED.
inline std::uint8_t take_stopped(const ble::ControlRequest &request) {
  auto &entry = stopped[slot(request.link, request.peer)];
  const std::uint8_t op =
      entry.generation == request.link_generation ? entry.op : 0;
  entry = Stopped{};
  return op;
}
} // namespace aqsync::abort
