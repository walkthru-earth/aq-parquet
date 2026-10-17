#include "abort_signal.h"
#include "aq_logger_transfer_gate.h"
#include "archive_sync.h"
#include "sync_codec.h"

#include <array>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>

namespace {
using namespace aq::sync;
std::size_t slot(ble::Link link, std::uint8_t peer = 0) {
  return link == ble::Link::Ble ? 0 : 1 + peer;
}
// abort::sequences and abort::stopped are process-wide; every ABORT case
// starts from a clean slate so earlier cases cannot leak a pending abort.
void reset_abort() {
  for (auto &sequence : aqsync::abort::sequences)
    sequence.store(0);
  for (auto &entry : aqsync::abort::stopped)
    entry = aqsync::abort::Stopped{};
}
// What a transport task does when an ABORT arrives on (link, peer).
ble::ControlRequest stamp_abort(ble::Link link, std::uint8_t peer,
                                std::uint32_t generation) {
  ble::ControlRequest marker;
  marker.link = link;
  marker.peer = peer;
  marker.link_generation = generation;
  marker.bytes[0] = ble::kOpAbort;
  marker.length = 1;
  aqsync::abort::stamp(marker);
  aqsync::abort::commit(marker);
  return marker;
}
unsigned descriptor_count() {
  unsigned count = 0;
  for (int descriptor = 0; descriptor < 1024; ++descriptor)
    if (::fcntl(descriptor, F_GETFD) >= 0)
      ++count;
  return count;
}
// Fills every descriptor below the highest one in use and lowers the soft
// limit to it, so the next open fails with EMFILE until restore().
struct DescriptorLimit {
  rlimit original{};
  std::vector<int> fillers;
  DescriptorLimit() {
    assert(::getrlimit(RLIMIT_NOFILE, &original) == 0);
    int highest = 0;
    for (int descriptor = 0; descriptor < 1024; ++descriptor)
      if (::fcntl(descriptor, F_GETFD) >= 0)
        highest = descriptor;
    for (int descriptor = 0; descriptor < highest; ++descriptor)
      if (::fcntl(descriptor, F_GETFD) < 0) {
        assert(::dup2(0, descriptor) == descriptor);
        fillers.push_back(descriptor);
      }
    rlimit lowered = original;
    lowered.rlim_cur = static_cast<rlim_t>(highest + 1);
    assert(::setrlimit(RLIMIT_NOFILE, &lowered) == 0);
  }
  ~DescriptorLimit() {
    assert(::setrlimit(RLIMIT_NOFILE, &original) == 0);
    for (const int descriptor : fillers)
      ::close(descriptor);
  }
};
struct Frame {
  ble::Link link;
  std::uint8_t peer;
  std::vector<std::uint8_t> bytes;
};
struct Error {
  ble::Link link;
  std::uint8_t peer;
  ble::Op op;
  ble::Error code;
  std::string detail;
};
struct Fixture {
  std::string directory;
  std::vector<std::uint8_t> bytes;
  std::vector<Frame> frames;
  std::vector<Error> errors;
  std::array<std::uint32_t, 1 + ble::kMaxLanClients> generation{1, 1, 1, 1};
  std::array<std::uint16_t, 1 + ble::kMaxLanClients> payload{517, 1024, 1024,
                                                             1024};
  std::vector<std::string> names{"sample.parquet"};
  unsigned partials = 70000;
  unsigned list_repeats = 1;
  unsigned listed = 0, progress = 0, locks = 0, lock_depth = 0;
  unsigned checks = 0, buffer_requests = 0;
  std::int64_t clock_us = 0;
  std::array<std::array<char, aqsync::ArchiveSession::kReadBufferBytes>,
             aqsync::ArchiveSession::kReadHandles>
      buffers{};
  bool complete = true;
  bool cache_hit = false;
  // Makes the finalized hook fail as if a read hit an I/O error.
  bool check_io_error = false;
  bool listing_complete = true;
  // Resumable fake walk over list_repeats x names. list_open is true while
  // the walk holds its directories between steps.
  std::size_t list_cursor = 0, list_reads = 0, list_steps = 0;
  std::size_t list_cancels = 0, list_budget_max = 0;
  bool list_open = false;
  // Runs between steps, as the storage worker's sample pass does.
  void (*between_steps)(Fixture &) = nullptr;
  std::size_t refuse_frame = 0;
  std::size_t disconnect_frame = 0;
  // Stamp an ABORT on (abort_link, abort_peer) as this frame is delivered.
  std::size_t abort_frame = 0;
  ble::Link abort_link = ble::Link::Ble;
  std::uint8_t abort_peer = 0;
  std::vector<std::string> logs;
  // The logger's battery close gate, run in front of the transport as the
  // firmware deliver hook does. The intent is raised once raise_close_after
  // frames were delivered, so 0 raises it before the first frame.
  aqlogger::power::EmergencyClose *close = nullptr;
  std::size_t raise_close_after = 0;

  explicit Fixture(const char *path) : directory(path), bytes(20000) {
    for (std::size_t i = 0; i < bytes.size(); ++i)
      bytes[i] = static_cast<std::uint8_t>(i);
    std::memcpy(bytes.data(), "PAR1", 4);
    put_u32(bytes.data() + bytes.size() - 8, 32);
    std::memcpy(bytes.data() + bytes.size() - 4, "PAR1", 4);
    FILE *file = std::fopen((directory + "/sample.parquet").c_str(), "wb");
    assert(file);
    assert(std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
    assert(std::fclose(file) == 0);
  }
  void clear() {
    frames.clear();
    errors.clear();
    refuse_frame = 0;
    disconnect_frame = 0;
    abort_frame = 0;
    abort_link = ble::Link::Ble;
    abort_peer = 0;
    logs.clear();
  }
  aqsync::ArchiveHooks archive() {
    return {
        this,
        [](void *ctx, bool begin, unsigned budget, aqsync::FileEmitter emit,
           void *out, unsigned &partials) {
          auto &f = *static_cast<Fixture *>(ctx);
          assert(budget > 0);
          ++f.list_steps;
          if (budget > f.list_budget_max)
            f.list_budget_max = budget;
          if (begin) {
            f.list_cursor = 0;
            f.list_open = true;
          }
          assert(f.list_open);
          const std::size_t total = f.list_repeats * f.names.size();
          for (unsigned read = 0; read < budget; ++read) {
            if (f.list_cursor == total) {
              f.list_open = false;
              if (!f.listing_complete)
                return aqsync::ListStatus::Failed;
              partials += f.partials;
              return aqsync::ListStatus::Complete;
            }
            ++f.list_reads;
            const auto &name = f.names[f.list_cursor++ % f.names.size()];
            if (!emit(out, name.c_str(), f.bytes.size())) {
              f.list_open = false;
              return aqsync::ListStatus::Stopped;
            }
          }
          return aqsync::ListStatus::More;
        },
        [](void *ctx, const char *name, char *path, std::size_t size) {
          auto &f = *static_cast<Fixture *>(ctx);
          if (!safe_parquet_name(name))
            return false;
          const int n =
              std::snprintf(path, size, "%s/%s", f.directory.c_str(), name);
          return n > 0 && static_cast<std::size_t>(n) < size;
        },
        // Reads the stream OPEN handed over, as the device owner does on a
        // cache miss. The session must rewind it before READ.
        [](void *ctx, const char *, FILE *file, aqsync::FileCheck &check) {
          auto &f = *static_cast<Fixture *>(ctx);
          assert(f.lock_depth == 0 && file);
          ++f.checks;
          std::vector<std::uint8_t> data(f.bytes.size() + 1);
          const std::size_t count =
              std::fread(data.data(), 1, data.size(), file);
          check.size = static_cast<std::uint32_t>(count);
          check.crc =
              crc32_update(0xffffffffU, data.data(), count) ^ 0xffffffffU;
          check.cached = f.cache_hit;
          if (f.check_io_error) {
            check.io_error = true;
            return false;
          }
          return f.complete && count >= 12 &&
                 parquet_complete(count, data.data(), data.data() + count - 8);
        },
        [](void *) { return std::uint32_t(123); },
        [](void *) { return std::uint32_t(45); },
        [](void *ctx) { ++static_cast<Fixture *>(ctx)->listed; },
        [](void *ctx) {
          auto &f = *static_cast<Fixture *>(ctx);
          assert(f.lock_depth++ == 0);
          ++f.locks;
        },
        [](void *ctx) {
          auto &f = *static_cast<Fixture *>(ctx);
          assert(f.lock_depth-- == 1);
        },
        [](void *ctx, const char *line) {
          static_cast<Fixture *>(ctx)->logs.push_back(line);
        },
        [](void *ctx, std::size_t index) -> void * {
          auto &f = *static_cast<Fixture *>(ctx);
          assert(index < f.buffers.size() && f.lock_depth == 1);
          ++f.buffer_requests;
          return f.buffers[index].data();
        },
        [](void *ctx) { return static_cast<Fixture *>(ctx)->clock_us += 250; },
        [](void *ctx) {
          auto &f = *static_cast<Fixture *>(ctx);
          ++f.list_cancels;
          f.list_open = false;
        }};
  }
  aqsync::ArchiveTransport transport() {
    return {this,
            [](void *ctx, ble::Link link, std::uint8_t peer) {
              return static_cast<Fixture *>(ctx)->generation[slot(link, peer)];
            },
            [](void *ctx, ble::Link link, std::uint8_t peer) {
              return static_cast<Fixture *>(ctx)->payload[slot(link, peer)];
            },
            [](void *ctx, ble::Link link, std::uint8_t peer,
               std::uint32_t generation, const std::uint8_t *data,
               std::size_t size) {
              auto &f = *static_cast<Fixture *>(ctx);
              if (f.close && f.frames.size() == f.raise_close_after)
                f.close->request();
              if (f.close &&
                  aqlogger::power::refuses_frame(*f.close, data, size, 0))
                return false;
              if (generation != f.generation[slot(link, peer)])
                return false;
              f.frames.push_back({link, peer, {data, data + size}});
              if (f.disconnect_frame && f.frames.size() == f.disconnect_frame)
                ++f.generation[slot(link, peer)];
              if (f.abort_frame && f.frames.size() == f.abort_frame)
                stamp_abort(f.abort_link, f.abort_peer,
                            f.generation[slot(f.abort_link, f.abort_peer)]);
              return !f.refuse_frame || f.frames.size() != f.refuse_frame;
            },
            [](void *ctx, ble::Link link, std::uint8_t peer,
               std::uint32_t generation, ble::Op op, ble::Error error,
               const char *detail) {
              auto &f = *static_cast<Fixture *>(ctx);
              assert(3 + (detail ? std::strlen(detail) : 0) <=
                     f.payload[slot(link, peer)]);
              if (generation != f.generation[slot(link, peer)])
                return false;
              f.errors.push_back({link, peer, op, error, detail ? detail : ""});
              return true;
            },
            [](void *ctx) { ++static_cast<Fixture *>(ctx)->progress; }};
  }
};
// Handles a request as the storage worker does, then runs a LIST job it
// started to its end, one bounded step per worker pass.
bool serve(aqsync::ArchiveSession &session, const ble::ControlRequest &r,
           bool storage_ready, Fixture *f = nullptr) {
  const bool handled = session.handle(r, storage_ready);
  while (session.listing()) {
    session.step(storage_ready);
    if (f && f->between_steps)
      f->between_steps(*f);
  }
  return handled;
}
ble::ControlRequest request(Fixture &f, ble::Op op,
                            ble::Link link = ble::Link::Ble,
                            std::uint8_t peer = 0) {
  ble::ControlRequest r;
  r.link = link;
  r.peer = peer;
  r.link_generation = f.generation[slot(link, peer)];
  r.bytes[0] = op;
  r.length = 1;
  return r;
}
std::uint16_t open(Fixture &f, aqsync::ArchiveSession &session,
                   ble::Link link = ble::Link::Ble,
                   const char *name = "sample.parquet", std::uint8_t peer = 0) {
  f.clear();
  auto r = request(f, ble::kOpOpen, link, peer);
  std::memcpy(r.bytes + 1, name, std::strlen(name));
  r.length += std::strlen(name);
  assert(session.handle(r, true));
  assert(f.errors.empty());
  assert(f.frames.size() == 1 && f.frames[0].link == link &&
         f.frames[0].peer == peer);
  const auto &frame = f.frames[0].bytes;
  assert(frame[0] == ble::kFrameOpened &&
         frame.size() == 11 + std::strlen(name));
  assert(get_u32(frame.data() + 3) == f.bytes.size());
  assert(
      get_u32(frame.data() + 7) ==
      (crc_update(0xffffffffU, f.bytes.data(), f.bytes.size()) ^ 0xffffffffU));
  assert(std::memcmp(frame.data() + 11, name, std::strlen(name)) == 0);
  return get_u16(frame.data() + 1);
}
ble::ControlRequest read(Fixture &f, std::uint16_t handle, std::uint32_t offset,
                         std::uint32_t count, ble::Link link = ble::Link::Ble,
                         std::uint8_t peer = 0) {
  auto r = request(f, ble::kOpRead, link, peer);
  r.length = 11;
  put_u16(r.bytes + 1, handle);
  put_u32(r.bytes + 3, offset);
  put_u32(r.bytes + 7, count);
  return r;
}
void expect_error(const Fixture &f, ble::Error code,
                  ble::Link link = ble::Link::Ble, std::uint8_t peer = 0) {
  assert(f.errors.size() == 1 && f.errors.back().code == code &&
         f.errors.back().link == link && f.errors.back().peer == peer);
  assert(f.frames.empty());
}
void verify_read(Fixture &f, std::uint16_t handle, std::uint32_t offset,
                 std::uint32_t count, std::size_t max_frame,
                 ble::Link link = ble::Link::Ble, std::uint8_t peer = 0) {
  assert(f.errors.empty());
  std::vector<std::uint8_t> received;
  for (std::size_t i = 0; i + 1 < f.frames.size(); ++i) {
    const auto &frame = f.frames[i];
    assert(frame.link == link && frame.peer == peer &&
           frame.bytes.size() <= max_frame);
    assert(frame.bytes[0] == ble::kFrameChunk);
    assert(get_u16(frame.bytes.data() + 1) == handle);
    assert(get_u32(frame.bytes.data() + 3) == offset + received.size());
    received.insert(received.end(), frame.bytes.begin() + 7, frame.bytes.end());
  }
  assert(received.size() == count);
  assert(std::memcmp(received.data(), f.bytes.data() + offset, count) == 0);
  const auto &end = f.frames.back();
  assert(end.link == link && end.peer == peer && end.bytes.size() == 8 &&
         end.bytes[0] == ble::kFrameReadEnd && end.bytes[7] == 0);
  assert(get_u16(end.bytes.data() + 1) == handle);
  assert(get_u32(end.bytes.data() + 3) == offset + count);
}

// ABORT (abort_signal.h): transports stamp each request, an ABORT advances the
// link's sequence, and the worker stops what the ABORT overtook.
void abort_cases(Fixture &f, aqsync::ArchiveSession &session) {
  namespace abort = aqsync::abort;
  const auto stamped = [&](ble::ControlRequest r) {
    abort::stamp(r);
    abort::commit(r);
    return r;
  };
  const auto expect_aborted = [&](ble::Op op, ble::Link link,
                                  std::uint8_t peer) {
    assert(f.errors.size() == 1 && f.errors[0].op == op &&
           f.errors[0].code == ble::kErrAborted && f.errors[0].link == link &&
           f.errors[0].peer == peer && f.errors[0].detail.empty());
  };
  f.payload = {517, 1024, 1024, 1024};
  f.list_repeats = 5;
  const unsigned listed = f.listed;

  // A LIST queued before an ABORT on its link never starts: ERROR aborted
  // only, then the marker names LIST once and a second marker names nothing.
  reset_abort();
  f.clear();
  const auto queued = stamped(request(f, ble::kOpList));
  stamp_abort(ble::Link::Ble, 0, f.generation[0]);
  assert(serve(session, queued, true));
  expect_aborted(ble::kOpList, ble::Link::Ble, 0);
  assert(f.frames.empty() && f.logs.empty() && f.listed == listed);
  auto marker = stamp_abort(ble::Link::Ble, 0, f.generation[0]);
  assert(abort::take_stopped(marker) == ble::kOpList);
  assert(abort::take_stopped(marker) == 0);

  // ABORT mid LIST: FILE frames stop, ERROR aborted and no LIST_END.
  for (const auto link : {ble::Link::Ble, ble::Link::Lan}) {
    reset_abort();
    f.clear();
    f.abort_frame = 2;
    f.abort_link = link;
    serve(session, stamped(request(f, ble::kOpList, link)), true);
    assert(f.frames.size() == 2);
    for (const auto &frame : f.frames)
      assert(frame.bytes[0] == ble::kFrameFile);
    expect_aborted(ble::kOpList, link, 0);
    assert(f.listed == listed);
    assert(f.logs.size() == 1);
    assert(f.logs[0].find("LIST ABORT") != std::string::npos &&
           f.logs[0].find("reason=aborted") != std::string::npos &&
           f.logs[0].find("after=2") != std::string::npos &&
           f.logs[0].find(link == ble::Link::Lan ? "link=lan" : "link=ble") !=
               std::string::npos);
    marker = stamp_abort(link, 0, f.generation[slot(link)]);
    assert(abort::take_stopped(marker) == ble::kOpList);
  }
  // The transport refusing the frame the ABORT overtook is still aborted, not
  // busy.
  reset_abort();
  f.clear();
  f.abort_frame = f.refuse_frame = 3;
  serve(session, stamped(request(f, ble::kOpList)), true);
  assert(f.frames.size() == 3);
  expect_aborted(ble::kOpList, ble::Link::Ble, 0);
  assert(f.logs.size() == 1 &&
         f.logs[0].find("after=2 reason=aborted") != std::string::npos);

  // ABORT mid READ: CHUNKs stop, ERROR aborted and no READ_END, both when the
  // overtaken frame was delivered and when the transport refused it.
  reset_abort();
  const auto handle = open(f, session, ble::Link::Lan, "sample.parquet", 2);
  for (const std::size_t refuse : {std::size_t(0), std::size_t(2)}) {
    reset_abort();
    f.clear();
    f.abort_frame = 2;
    f.refuse_frame = refuse;
    f.abort_link = ble::Link::Lan;
    f.abort_peer = 2;
    serve(session, stamped(read(f, handle, 0, 8000, ble::Link::Lan, 2)), true);
    assert(f.frames.size() == 2);
    for (const auto &frame : f.frames)
      assert(frame.bytes[0] == ble::kFrameChunk && frame.peer == 2);
    expect_aborted(ble::kOpRead, ble::Link::Lan, 2);
    marker =
        stamp_abort(ble::Link::Lan, 2, f.generation[slot(ble::Link::Lan, 2)]);
    assert(abort::take_stopped(marker) == ble::kOpRead);
  }
  // A READ queued before the ABORT never sends a CHUNK.
  reset_abort();
  f.clear();
  const auto queued_read = stamped(read(f, handle, 0, 100, ble::Link::Lan, 2));
  stamp_abort(ble::Link::Lan, 2, f.generation[slot(ble::Link::Lan, 2)]);
  serve(session, queued_read, true);
  expect_aborted(ble::kOpRead, ble::Link::Lan, 2);
  assert(f.frames.empty());
  // The handle survives the ABORT; a fresh READ after it runs normally.
  f.clear();
  serve(session, stamped(read(f, handle, 10, 3000, ble::Link::Lan, 2)), true);
  verify_read(f, handle, 10, 3000, 1024, ble::Link::Lan, 2);

  // Stale marker: the stop was recorded under generation g, the marker
  // arrives for the replacement connection g+1 and names nothing.
  reset_abort();
  f.clear();
  const auto old_list = stamped(request(f, ble::kOpList, ble::Link::Lan, 1));
  stamp_abort(ble::Link::Lan, 1, f.generation[slot(ble::Link::Lan, 1)]);
  serve(session, old_list, true);
  expect_aborted(ble::kOpList, ble::Link::Lan, 1);
  marker =
      stamp_abort(ble::Link::Lan, 1, f.generation[slot(ble::Link::Lan, 1)] + 1);
  assert(abort::take_stopped(marker) == 0);

  // Abort slots are per link and per LAN peer: an ABORT on BLE or on another
  // LAN peer neither stops nor is named by this peer's LIST and READ.
  reset_abort();
  f.clear();
  const auto other_list = stamped(request(f, ble::kOpList, ble::Link::Lan, 2));
  stamp_abort(ble::Link::Ble, 0, f.generation[0]);
  stamp_abort(ble::Link::Lan, 0, f.generation[slot(ble::Link::Lan, 0)]);
  stamp_abort(ble::Link::Lan, 1, f.generation[slot(ble::Link::Lan, 1)]);
  serve(session, other_list, true);
  assert(f.errors.empty() && f.frames.size() == 6 &&
         f.frames.back().bytes[0] == ble::kFrameListEnd &&
         get_u32(f.frames.back().bytes.data() + 1) == 5);
  f.clear();
  f.abort_frame = 1;
  f.abort_link = ble::Link::Lan;
  f.abort_peer = 1;
  serve(session, stamped(read(f, handle, 0, 3000, ble::Link::Lan, 2)), true);
  verify_read(f, handle, 0, 3000, 1024, ble::Link::Lan, 2);
  f.clear();
  f.abort_frame = 1;
  f.abort_link = ble::Link::Ble;
  serve(session, stamped(request(f, ble::kOpList, ble::Link::Lan, 2)), true);
  assert(f.errors.empty() && f.frames.back().bytes[0] == ble::kFrameListEnd);
  for (std::uint8_t peer = 0; peer < 3; ++peer) {
    const auto link = peer ? ble::Link::Lan : ble::Link::Ble;
    const auto other = peer ? std::uint8_t(peer - 1) : std::uint8_t(0);
    marker = stamp_abort(link, other, f.generation[slot(link, other)]);
    assert(abort::take_stopped(marker) == 0);
  }
  reset_abort();
  f.clear();
  f.list_repeats = 1;
}
// READ windows reuse the descriptor OPEN created, release_files() drops
// descriptors but keeps handles, and descriptor exhaustion is OpenFailed.
void persistent_cases(Fixture &f, aqsync::ArchiveSession &session,
                      unsigned idle_descriptors) {
  const std::string path = f.directory + "/sample.parquet";
  const std::string moved = f.directory + "/moved.parquet";
  const unsigned checks = f.checks, buffers = f.buffer_requests;
  const auto lan = open(f, session, ble::Link::Lan, "sample.parquet", 1);
  const auto &handle = lan;
  assert(f.checks == checks + 1 && f.buffer_requests == buffers + 1);
  assert(f.logs.size() == 1 &&
         f.logs[0].find("crc=miss open_us=250 ") != std::string::npos);
  // With the path gone, READ still serves every window from the open stream.
  assert(std::rename(path.c_str(), moved.c_str()) == 0);
  for (std::uint32_t offset = 0; offset < f.bytes.size(); offset += 4096) {
    const std::uint32_t count =
        f.bytes.size() - offset < 4096 ? f.bytes.size() - offset : 4096;
    f.clear();
    serve(session, read(f, handle, offset, count, ble::Link::Lan, 1), true);
    verify_read(f, handle, offset, count, 1024, ble::Link::Lan, 1);
    assert(descriptor_count() == idle_descriptors + 1);
  }
  // release_files() closes the stream; a READ while the path is gone fails
  // and the handle stays valid for a retry once the path is back.
  session.release_files();
  assert(descriptor_count() == idle_descriptors);
  f.clear();
  serve(session, read(f, handle, 0, 10, ble::Link::Lan, 1), true);
  assert(f.errors.empty() && f.frames.size() == 1 &&
         f.frames[0].bytes[0] == ble::kFrameReadEnd &&
         f.frames[0].bytes[7] == ble::kErrOpenFailed);
  assert(descriptor_count() == idle_descriptors);
  assert(std::rename(moved.c_str(), path.c_str()) == 0);
  f.clear();
  serve(session, read(f, handle, 333, 1500, ble::Link::Lan, 1), true);
  verify_read(f, handle, 333, 1500, 1024, ble::Link::Lan, 1);
  assert(descriptor_count() == idle_descriptors + 1);
  assert(f.checks == checks + 1 && f.buffer_requests == buffers + 2);

  // A cached CRC is reported in the OPEN log line.
  f.cache_hit = true;
  open(f, session);
  assert(f.logs.size() == 1 &&
         f.logs[0].find("crc=hit open_us=250 ") != std::string::npos);
  f.cache_hit = false;
  assert(descriptor_count() == idle_descriptors + 2);

  // A failed check and a missing path leave no descriptor behind.
  f.clear();
  f.complete = false;
  auto rejected = request(f, ble::kOpOpen);
  const char *name = "sample.parquet";
  std::memcpy(rejected.bytes + 1, name, std::strlen(name));
  rejected.length += std::strlen(name);
  serve(session, rejected, true);
  expect_error(f, ble::kErrNotFinalized);
  f.complete = true;
  assert(descriptor_count() == idle_descriptors + 1);
  f.clear();
  const unsigned before_missing = f.checks;
  auto missing = request(f, ble::kOpOpen);
  const char *absent = "absent.parquet";
  std::memcpy(missing.bytes + 1, absent, std::strlen(absent));
  missing.length += std::strlen(absent);
  serve(session, missing, true);
  expect_error(f, ble::kErrNotFinalized);
  assert(f.checks == before_missing);
  assert(descriptor_count() == idle_descriptors + 1);

  // A full descriptor table is OpenFailed, not NotFinalized, and the open
  // LAN handle keeps reading through its existing descriptor.
  {
    DescriptorLimit limit;
    f.clear();
    serve(session, rejected, true);
    expect_error(f, ble::kErrOpenFailed);
    assert(f.logs.size() == 1 && f.logs[0].find("BLE OPEN FAIL errno=") == 0);
    f.clear();
    serve(session, read(f, lan, 4000, 2000, ble::Link::Lan, 1), true);
    verify_read(f, lan, 4000, 2000, 1024, ble::Link::Lan, 1);
  }
  session.close();
  assert(descriptor_count() == idle_descriptors);
}
// remounted() closes every stream and the next READ on each handle checks
// the reopened file again once mounted() follows. Same bytes keep serving. A
// proven change is ERROR bad handle and the handle is gone. An unmounted
// volume or an I/O error is READ_END status 4 and the recheck stays pending.
void write_file(const std::string &path,
                const std::vector<std::uint8_t> &data) {
  FILE *file = std::fopen(path.c_str(), "wb");
  assert(file);
  assert(std::fwrite(data.data(), 1, data.size(), file) == data.size());
  assert(std::fclose(file) == 0);
}
std::vector<std::uint8_t> other_parquet(std::size_t size) {
  std::vector<std::uint8_t> data(size, 0x5a);
  std::memcpy(data.data(), "PAR1", 4);
  put_u32(data.data() + size - 8, 32);
  std::memcpy(data.data() + size - 4, "PAR1", 4);
  return data;
}
void expect_bad_handle_read(Fixture &f, aqsync::ArchiveSession &session,
                            std::uint16_t handle) {
  f.clear();
  serve(session, read(f, handle, 0, 10), true);
  assert(f.errors.size() == 1 && f.errors[0].op == ble::kOpRead);
  expect_error(f, ble::kErrBadHandle);
}
// A remount the board completed, as probe_storage reports it.
void remount(aqsync::ArchiveSession &session) {
  session.remounted();
  session.mounted();
}
// READ_END status 4 at offset with no CHUNK, the retryable open failure.
void expect_open_failed_read(Fixture &f, aqsync::ArchiveSession &session,
                             std::uint16_t handle, std::uint32_t offset) {
  f.clear();
  serve(session, read(f, handle, offset, 10), true);
  assert(f.errors.empty() && f.frames.size() == 1);
  const auto &end = f.frames[0].bytes;
  assert(end.size() == 8 && end[0] == ble::kFrameReadEnd &&
         get_u16(end.data() + 1) == handle &&
         get_u32(end.data() + 3) == offset && end[7] == ble::kErrOpenFailed);
}
void remount_cases(Fixture &f, aqsync::ArchiveSession &session,
                   unsigned idle_descriptors) {
  const std::string path = f.directory + "/sample.parquet";
  unsigned checks = f.checks;

  // Identical bytes after a remount: one check, then plain descriptor reuse.
  auto handle = open(f, session);
  assert(f.checks == ++checks);
  remount(session);
  assert(descriptor_count() == idle_descriptors);
  f.clear();
  serve(session, read(f, handle, 100, 3000), true);
  verify_read(f, handle, 100, 3000, 512);
  assert(f.checks == ++checks);
  assert(f.logs.size() == 2 &&
         f.logs[0].find("BLE READ RECHECK handle=") == 0 &&
         f.logs[0].find(" result=same check_us=") != std::string::npos);
  f.clear();
  serve(session, read(f, handle, 5000, 2000), true);
  verify_read(f, handle, 5000, 2000, 512);
  assert(f.checks == checks);
  // A later release_files() on the same mount reopens without a check.
  session.release_files();
  f.clear();
  serve(session, read(f, handle, 0, 10), true);
  verify_read(f, handle, 0, 10, 512);
  assert(f.checks == checks && descriptor_count() == idle_descriptors + 1);

  // Repeated remounts need one check, and each new remount needs another.
  // The flag has no counter, so no epoch can wrap back to a match.
  for (int round = 0; round < 2; ++round) {
    session.remounted();
    session.remounted();
    remount(session);
    f.clear();
    serve(session, read(f, handle, 64, 64), true);
    verify_read(f, handle, 64, 64, 512);
    assert(f.checks == ++checks);
  }

  // A full descriptor table after a remount is a retryable READ_END, and the
  // check still runs once the file opens again.
  remount(session);
  {
    DescriptorLimit limit;
    f.clear();
    serve(session, read(f, handle, 0, 10), true);
    assert(f.errors.empty() && f.frames.size() == 1 &&
           f.frames[0].bytes[0] == ble::kFrameReadEnd &&
           f.frames[0].bytes[7] == ble::kErrOpenFailed);
    assert(f.checks == checks);
  }
  f.clear();
  serve(session, read(f, handle, 0, 10), true);
  verify_read(f, handle, 0, 10, 512);
  assert(f.checks == ++checks);

  // Different bytes and size at the same path drop the handle.
  remount(session);
  write_file(path, other_parquet(f.bytes.size() + 777));
  expect_bad_handle_read(f, session, handle);
  assert(f.checks == ++checks);
  assert(f.logs.size() == 1 &&
         f.logs[0].find(" result=changed ") != std::string::npos);
  assert(descriptor_count() == idle_descriptors);
  expect_bad_handle_read(f, session, handle); // gone, not retried
  assert(f.checks == checks);
  f.clear();
  auto close_request = request(f, ble::kOpClose);
  close_request.length = 3;
  put_u16(close_request.bytes + 1, handle);
  serve(session, close_request, true);
  expect_error(f, ble::kErrBadHandle);
  write_file(path, f.bytes);

  // Same size but different bytes is a CRC mismatch, also bad handle.
  handle = open(f, session);
  assert(f.checks == ++checks);
  remount(session);
  write_file(path, other_parquet(f.bytes.size()));
  expect_bad_handle_read(f, session, handle);
  assert(f.checks == ++checks);
  write_file(path, f.bytes);

  // A file that is no longer finalized drops the handle.
  handle = open(f, session);
  assert(f.checks == ++checks);
  remount(session);
  f.complete = false;
  expect_bad_handle_read(f, session, handle);
  assert(f.checks == ++checks);
  f.complete = true;

  // A file deleted while the card was out drops the handle without a check.
  handle = open(f, session);
  assert(f.checks == ++checks);
  remount(session);
  assert(std::remove(path.c_str()) == 0);
  expect_bad_handle_read(f, session, handle);
  assert(f.checks == checks && descriptor_count() == idle_descriptors);
  write_file(path, f.bytes);

  // The other slots are checked independently, a fresh OPEN needs no recheck.
  const auto lan = open(f, session, ble::Link::Lan, "sample.parquet", 2);
  handle = open(f, session);
  checks += 2;
  remount(session);
  handle = open(f, session);
  assert(f.checks == ++checks);
  f.clear();
  serve(session, read(f, handle, 0, 10), true);
  verify_read(f, handle, 0, 10, 512);
  assert(f.checks == checks);
  f.clear();
  serve(session, read(f, lan, 0, 10, ble::Link::Lan, 2), true);
  verify_read(f, lan, 0, 10, 1024, ble::Link::Lan, 2);
  assert(f.checks == ++checks);
  session.close();
  assert(descriptor_count() == idle_descriptors);

  // CLOSE on a handle still waiting for its recheck answers CLOSED and frees
  // the slot without a check.
  handle = open(f, session);
  assert(f.checks == ++checks);
  remount(session);
  f.clear();
  close_request = request(f, ble::kOpClose);
  close_request.length = 3;
  put_u16(close_request.bytes + 1, handle);
  serve(session, close_request, true);
  assert(f.errors.empty() && f.frames.size() == 1 &&
         f.frames[0].bytes[0] == ble::kFrameClosed &&
         get_u16(f.frames[0].bytes.data() + 1) == handle);
  assert(f.checks == checks && descriptor_count() == idle_descriptors);
  expect_bad_handle_read(f, session, handle);
  assert(f.checks == checks);

  // A generation change while a recheck is pending clears the slot. The old
  // handle answers bad handle without a check and a fresh OPEN needs none.
  handle = open(f, session);
  assert(f.checks == ++checks);
  remount(session);
  ++f.generation[slot(ble::Link::Ble)];
  expect_bad_handle_read(f, session, handle);
  assert(f.checks == checks && descriptor_count() == idle_descriptors);
  handle = open(f, session);
  assert(f.checks == ++checks);
  f.clear();
  serve(session, read(f, handle, 0, 10), true);
  verify_read(f, handle, 0, 10, 512);
  assert(f.checks == checks);

  // A volume that did not mount again answers every path as missing, so the
  // file is moved away here too. READ is READ_END status 4 without opening
  // anything, and the recheck waits, also across another failed remount.
  const std::string away = path + ".away";
  session.remounted();
  assert(std::rename(path.c_str(), away.c_str()) == 0);
  expect_open_failed_read(f, session, handle, 100);
  expect_open_failed_read(f, session, handle, 100);
  session.remounted();
  expect_open_failed_read(f, session, handle, 100);
  assert(f.checks == checks && descriptor_count() == idle_descriptors);
  // The same card mounts again unchanged and the handle serves.
  assert(std::rename(away.c_str(), path.c_str()) == 0);
  session.mounted();
  f.clear();
  serve(session, read(f, handle, 100, 3000), true);
  verify_read(f, handle, 100, 3000, 512);
  assert(f.checks == ++checks);

  // An I/O error during the recheck is READ_END status 4. The handle stays,
  // each retry checks again, and a clean check serves.
  remount(session);
  f.check_io_error = true;
  expect_open_failed_read(f, session, handle, 0);
  assert(f.checks == ++checks && descriptor_count() == idle_descriptors);
  assert(f.logs.size() == 1 &&
         f.logs[0].find(" result=io_error check_us=") != std::string::npos);
  expect_open_failed_read(f, session, handle, 0);
  assert(f.checks == ++checks);
  f.check_io_error = false;
  f.clear();
  serve(session, read(f, handle, 0, 10), true);
  verify_read(f, handle, 0, 10, 512);
  assert(f.checks == ++checks);

  // A refused CHUNK, as a critical battery close preempts it, comes after
  // the recheck. A same result is proof, so the handle keeps serving and the
  // retry needs no second check.
  remount(session);
  f.clear();
  f.refuse_frame = 1;
  session.handle(read(f, handle, 0, 3000), true);
  assert(f.checks == ++checks && f.errors.empty() && f.frames.size() == 2);
  {
    const auto &end = f.frames[1].bytes;
    assert(end.size() == 8 && end[0] == ble::kFrameReadEnd &&
           get_u32(end.data() + 3) == 0 && end[7] == ble::kErrBusy);
  }
  f.clear();
  session.handle(read(f, handle, 0, 3000), true);
  verify_read(f, handle, 0, 3000, 512);
  assert(f.checks == checks);
  // A change proven before the first CHUNK ends the handle with ERROR bad
  // handle, a frame the preempt never refuses.
  remount(session);
  write_file(path, other_parquet(f.bytes.size()));
  f.clear();
  f.refuse_frame = 1;
  session.handle(read(f, handle, 0, 10), true);
  expect_error(f, ble::kErrBadHandle);
  assert(f.checks == ++checks);
  write_file(path, f.bytes);

  // A close raised while a READ is open, with storage in Retry. The READ
  // ends busy at its first CHUNK, then the worker's writable() probe
  // finalizes, remounts and reports remounted() and mounted(). The retried
  // READ rechecks before any byte instead of serving the old stream, so
  // unchanged bytes serve and a changed file ends the handle.
  for (const bool changed : {false, true}) {
    aqlogger::power::EmergencyClose close;
    handle = open(f, session);
    assert(f.checks == ++checks);
    f.clear();
    f.close = &close;
    f.raise_close_after = 0;
    session.handle(read(f, handle, 0, 3000), true);
    assert(f.errors.empty() && f.frames.size() == 1);
    {
      const auto &end = f.frames[0].bytes;
      assert(end.size() == 8 && end[0] == ble::kFrameReadEnd &&
             get_u32(end.data() + 3) == 0 && end[7] == ble::kErrBusy);
    }
    assert(f.checks == checks);
    assert(close.due(0));
    remount(session); // the Retry probe inside writable()
    assert(close.finish(aqlogger::power::CloseOutcome::Closed, 0) ==
           aqlogger::power::Next::Done);
    f.close = nullptr;
    if (changed) {
      write_file(path, other_parquet(f.bytes.size()));
      expect_bad_handle_read(f, session, handle);
      assert(f.checks == ++checks);
      write_file(path, f.bytes);
    } else {
      f.clear();
      session.handle(read(f, handle, 0, 3000), true);
      verify_read(f, handle, 0, 3000, 512);
      assert(f.checks == ++checks);
      assert(f.logs.size() == 2 &&
             f.logs[0].find(" result=same check_us=") != std::string::npos);
    }
  }
  session.close();
  assert(descriptor_count() == idle_descriptors);
}
} // namespace

// LIST runs as a job of bounded steps (issue #33). handle() starts it and
// the storage worker calls step() once per pass.
void list_job_cases(Fixture &f, aqsync::ArchiveSession &session) {
  namespace abort = aqsync::abort;
  constexpr auto kStep = aqsync::ArchiveSession::kListStepEntries;
  const auto stamped = [](ble::ControlRequest r) {
    abort::stamp(r);
    abort::commit(r);
    return r;
  };
  const auto file_names = [&] {
    std::vector<std::string> names;
    for (const auto &frame : f.frames)
      if (frame.bytes[0] == ble::kFrameFile)
        names.emplace_back(frame.bytes.begin() + 5, frame.bytes.end());
    return names;
  };
  const unsigned listed = f.listed;
  f.payload = {517, 1024, 1024, 1024};

  // handle() only starts the job. Nothing is read before the first step.
  reset_abort();
  f.clear();
  f.list_repeats = 1000;
  f.list_reads = f.list_steps = f.list_cancels = 0;
  const auto list = stamped(request(f, ble::kOpList));
  assert(session.handle(list, true));
  assert(session.listing() && f.list_reads == 0 && f.frames.empty());
  const auto *tag = session.list_tag();
  assert(tag && tag->link == ble::Link::Ble && tag->peer == 0 &&
         tag->generation == f.generation[0]);
  session.step(true);
  assert(session.listing() && f.list_reads == kStep &&
         f.frames.size() == kStep && f.list_open);

  // ABORT between steps stops the walk before its next read.
  stamp_abort(ble::Link::Ble, 0, f.generation[0]);
  const std::size_t reads = f.list_reads;
  session.step(true);
  assert(!session.listing() && !session.list_tag());
  assert(f.list_reads == reads && !f.list_open && f.list_cancels == 1);
  assert(f.frames.size() == kStep && f.errors.size() == 1 &&
         f.errors[0].code == ble::kErrAborted &&
         f.errors[0].op == ble::kOpList);
  auto marker = stamp_abort(ble::Link::Ble, 0, f.generation[0]);
  assert(abort::take_stopped(marker) == ble::kOpList);

  // ABORT during a step stops it at the next entry, within one step.
  reset_abort();
  f.clear();
  f.abort_frame = kStep + 7;
  f.abort_link = ble::Link::Lan;
  f.abort_peer = 1;
  std::size_t reads_at_abort = 0;
  f.list_reads = 0;
  session.handle(stamped(request(f, ble::kOpList, ble::Link::Lan, 1)), true);
  while (session.listing()) {
    session.step(true);
    if (!reads_at_abort && f.frames.size() >= f.abort_frame)
      reads_at_abort = f.frames.size();
  }
  assert(reads_at_abort == kStep + 7);
  assert(f.list_reads - reads_at_abort <= kStep && !f.list_open);
  assert(f.errors.size() == 1 && f.errors[0].code == ble::kErrAborted &&
         f.errors[0].link == ble::Link::Lan && f.errors[0].peer == 1);
  marker =
      stamp_abort(ble::Link::Lan, 1, f.generation[slot(ble::Link::Lan, 1)]);
  assert(abort::take_stopped(marker) == ble::kOpList);

  // The emitter refusing a frame stops the walk at that entry.
  reset_abort();
  f.clear();
  f.list_reads = 0;
  f.refuse_frame = kStep + 5;
  serve(session, request(f, ble::kOpList), true);
  assert(f.list_reads == kStep + 5 && !f.list_open);
  assert(f.frames.size() == kStep + 5 && f.errors.size() == 1 &&
         f.errors[0].code == ble::kErrBusy && f.errors[0].detail == "refused");
  assert(f.listed == listed);

  // Bounded steps list exactly what one unbounded pass lists, in order.
  f.clear();
  f.names = {"a.parquet", "b.parquet", "c.parquet", "d.parquet", "e.parquet"};
  f.list_repeats = 41;
  std::vector<std::string> unbounded;
  {
    unsigned partials = 0;
    const auto hooks = f.archive();
    const aqsync::FileEmitter collect = [](void *out, const char *name,
                                           std::uint32_t) {
      static_cast<std::vector<std::string> *>(out)->push_back(name);
      return true;
    };
    assert(hooks.list(hooks.context, true, UINT32_MAX, collect, &unbounded,
                      partials) == aqsync::ListStatus::Complete);
  }
  f.list_steps = 0;
  serve(session, request(f, ble::kOpList), true);
  assert(file_names() == unbounded && unbounded.size() == 205);
  assert(f.list_steps == (205 + kStep) / kStep);
  assert(f.frames.back().bytes[0] == ble::kFrameListEnd &&
         get_u32(f.frames.back().bytes.data() + 1) == 205);
  assert(f.errors.empty() && f.listed == listed + 1);
  f.names = {"sample.parquet"};

  // A huge directory never holds the worker for more than one step. The
  // worker's sample pass runs between steps and every frame is progress.
  f.clear();
  f.list_repeats = 10000;
  f.list_steps = f.list_budget_max = 0;
  f.progress = 0;
  static std::size_t passes;
  passes = 0;
  f.between_steps = [](Fixture &) { ++passes; };
  serve(session, request(f, ble::kOpList), true, &f);
  f.between_steps = nullptr;
  assert(f.list_budget_max == kStep);
  assert(f.list_steps == (10000 + kStep) / kStep && passes == f.list_steps);
  assert(f.progress == f.frames.size() && f.frames.size() == 10001);
  assert(f.listed == listed + 2);

  // While the job runs, requests on its link and peer and any other LIST
  // wait, and other links are served between steps.
  f.clear();
  f.list_repeats = 100;
  session.handle(request(f, ble::kOpList, ble::Link::Lan, 0), true);
  session.step(true);
  assert(session.must_wait_for_list(
      request(f, ble::kOpStatus, ble::Link::Lan, 0)));
  assert(session.must_wait_for_list(request(f, ble::kOpList)));
  auto marker_request = request(f, ble::kOpAbort, ble::Link::Lan, 0);
  assert(session.must_wait_for_list(marker_request));
  assert(!session.must_wait_for_list(request(f, ble::kOpStatus)));
  assert(
      !session.must_wait_for_list(request(f, ble::kOpRead, ble::Link::Lan, 2)));
  const auto other = open(f, session, ble::Link::Lan, "sample.parquet", 2);
  f.clear();
  session.handle(read(f, other, 0, 100, ble::Link::Lan, 2), true);
  assert(session.listing());
  assert(!f.frames.empty() && f.frames.front().peer == 2);
  // A second LIST reaching handle() anyway is refused, the job continues.
  f.clear();
  session.handle(request(f, ble::kOpList), true);
  assert(f.errors.size() == 1 && f.errors[0].code == ble::kErrBusy &&
         f.errors[0].link == ble::Link::Ble && f.errors[0].detail == "list");
  assert(session.listing());
  f.clear();
  while (session.listing())
    session.step(true);
  assert(f.errors.empty() && f.frames.back().bytes[0] == ble::kFrameListEnd &&
         f.frames.back().peer == 0 &&
         get_u32(f.frames.back().bytes.data() + 1) == 100);
  auto close_other = request(f, ble::kOpClose, ble::Link::Lan, 2);
  close_other.length = 3;
  put_u16(close_other.bytes + 1, other);
  serve(session, close_other, true);

  // A peer that left gets no frames, its walk just closes.
  f.clear();
  f.list_cancels = 0;
  session.handle(request(f, ble::kOpList, ble::Link::Lan, 1), true);
  session.step(true);
  const std::size_t sent = f.frames.size();
  ++f.generation[slot(ble::Link::Lan, 1)];
  session.step(true);
  assert(!session.listing() && !f.list_open && f.list_cancels == 1);
  assert(f.frames.size() == sent && f.errors.empty());

  // Storage leaving mid walk, a remount and an erase end the job with
  // ERROR storage and close the walk.
  for (int cause = 0; cause < 3; ++cause) {
    f.clear();
    session.handle(request(f, ble::kOpList), true);
    session.step(true);
    assert(f.list_open);
    if (cause == 0)
      session.step(false);
    else if (cause == 1)
      session.remounted();
    else
      session.close();
    if (cause == 1)
      session.mounted();
    assert(!session.listing() && !f.list_open);
    assert(f.errors.size() == 1 && f.errors[0].code == ble::kErrStorage &&
           f.errors[0].op == ble::kOpList &&
           f.errors[0].link == ble::Link::Ble);
  }
  f.list_repeats = 1;
  reset_abort();
  f.clear();
}

int main(int argc, char **argv) {
  assert(argc == 2);
  Fixture f(argv[1]);
  aqsync::ArchiveSession session(f.archive(), f.transport());

  assert(!serve(session, request(f, ble::kOpStatus), true));
  assert(serve(session, request(f, ble::kOpList), false));
  expect_error(f, ble::kErrStorage);
  f.clear();
  assert(serve(session, request(f, ble::kOpList), true));
  assert(f.frames.size() == 2 && f.listed == 1 && f.progress == 2);
  const auto &list = f.frames.back().bytes;
  assert(list.size() == 15 && list[0] == ble::kFrameListEnd);
  assert(get_u32(list.data() + 1) == 1 && get_u16(list.data() + 5) == 65535);
  assert(get_u32(list.data() + 7) == 123 && get_u32(list.data() + 11) == 45);
  f.clear();
  f.refuse_frame = 1;
  serve(session, request(f, ble::kOpList), true);
  assert(f.frames.size() == 1 && f.listed == 1); // no LIST_END after refusal
  // A refusal without ABORT still ends LIST with a terminal ERROR busy, so
  // the client never waits for a LIST_END that will not come.
  assert(f.errors.size() == 1 && f.errors[0].op == ble::kOpList &&
         f.errors[0].code == ble::kErrBusy && f.errors[0].detail == "refused");
  assert(f.logs.size() == 1 &&
         f.logs[0].find("LIST ABORT link=ble after=0 reason=refused") !=
             std::string::npos);
  f.clear();
  f.payload[0] = 18;
  serve(session, request(f, ble::kOpList), true);
  expect_error(f, ble::kErrBusy);
  assert(f.listed == 1); // no false empty listing or cleared new-files hint
  f.payload[0] = 517;

  // SD failure after FILE frames is an error, never a successful prefix.
  for (const auto link : {ble::Link::Ble, ble::Link::Lan}) {
    f.clear();
    f.listing_complete = false;
    serve(session, request(f, ble::kOpList, link), true);
    assert(f.frames.size() == 1 && f.frames[0].bytes[0] == ble::kFrameFile);
    assert(f.errors.size() == 1 && f.errors[0].op == ble::kOpList &&
           f.errors[0].code == ble::kErrStorage && f.errors[0].link == link);
    assert(f.listed == 1);
  }
  f.listing_complete = true;
  f.clear();
  f.refuse_frame = 2; // LIST_END delivery fails after a successful traversal.
  serve(session, request(f, ble::kOpList), true);
  assert(f.frames.size() == 2 && f.listed == 1);
  f.clear();
  serve(session, request(f, ble::kOpList, ble::Link::Lan), true);
  assert(f.frames.size() == 2 && f.listed == 2 && f.errors.empty());
  f.clear();
  f.list_repeats = 70000; // Proto 3 LIST_END count is u32, past the old cap.
  serve(session, request(f, ble::kOpList, ble::Link::Lan), true);
  assert(f.frames.size() == 70001 && f.listed == 3 && f.errors.empty());
  assert(f.frames.back().bytes[0] == ble::kFrameListEnd &&
         f.frames.back().bytes.size() == 15 &&
         get_u32(f.frames.back().bytes.data() + 1) == 70000);
  f.list_repeats = 1;
  // A critical battery close raised mid LIST stops it at the next FILE
  // through the logger's gate, with the same terminal ERROR busy. The worker
  // runs the close, then a retried LIST completes.
  {
    aqlogger::power::EmergencyClose close;
    f.clear();
    f.list_repeats = 3;
    f.close = &close;
    f.raise_close_after = 1;
    serve(session, request(f, ble::kOpList, ble::Link::Lan), true);
    assert(f.frames.size() == 1 && f.frames[0].bytes[0] == ble::kFrameFile);
    assert(f.errors.size() == 1 && f.errors[0].op == ble::kOpList &&
           f.errors[0].code == ble::kErrBusy &&
           f.errors[0].detail == "refused" &&
           f.errors[0].link == ble::Link::Lan);
    const unsigned listed_before = f.listed;
    assert(close.due(0) && close.finish(aqlogger::power::CloseOutcome::Closed,
                                        0) == aqlogger::power::Next::Done);
    f.clear();
    f.raise_close_after = static_cast<std::size_t>(-1);
    serve(session, request(f, ble::kOpList, ble::Link::Lan), true);
    assert(f.frames.size() == 4 && f.errors.empty() &&
           f.listed == listed_before + 1 &&
           f.frames.back().bytes[0] == ble::kFrameListEnd);
    f.close = nullptr;
    f.raise_close_after = 0;
    f.list_repeats = 1;
  }
  list_job_cases(f, session);

  auto handle = open(f, session);
  f.clear();
  f.payload[0] = 24; // LIST fits, but OPENED has a larger header
  auto too_small = request(f, ble::kOpOpen);
  const char *file_name = "sample.parquet";
  std::memcpy(too_small.bytes + 1, file_name, std::strlen(file_name));
  too_small.length += std::strlen(file_name);
  serve(session, too_small, true);
  expect_error(f, ble::kErrBusy);
  f.payload[0] = 517;
  f.clear();
  serve(session, read(f, handle, 0, 10), true);
  verify_read(f, handle, 0, 10, 512); // rejected OPEN preserves prior handle
  f.clear();
  serve(session, read(f, handle, 0, UINT32_MAX), true);
  verify_read(f, handle, 0, ble::kMaxRead, 512);
  f.clear();
  serve(session, read(f, handle, f.bytes.size() - 15, 200), true);
  verify_read(f, handle, f.bytes.size() - 15, 15, 512);
  f.clear();
  serve(session, read(f, handle, f.bytes.size() + 1, 1), true);
  expect_error(f, ble::kErrRange);
  f.clear();
  serve(session, read(f, handle, 0, 10, ble::Link::Lan), true);
  expect_error(f, ble::kErrBadHandle, ble::Link::Lan);
  ++f.generation[1]; // another transport reconnect cannot kill a BLE handle
  f.clear();
  serve(session, read(f, handle, 7, 22), true);
  verify_read(f, handle, 7, 22, 512);
  ++f.generation[0];
  f.clear();
  serve(session, read(f, handle, 0, 10), true);
  expect_error(f, ble::kErrBadHandle);

  handle = open(f, session, ble::Link::Lan);
  f.clear();
  serve(session, read(f, handle, 123, 2040, ble::Link::Lan), true);
  verify_read(f, handle, 123, 2040, 1024, ble::Link::Lan);
  f.clear();
  auto malformed = read(f, handle, 0, 10, ble::Link::Lan);
  --malformed.length;
  serve(session, malformed, true);
  expect_error(f, ble::kErrMalformed, ble::Link::Lan);
  f.clear();
  f.refuse_frame = 2;
  serve(session, read(f, handle, 0, 4000, ble::Link::Lan), true);
  assert(f.frames.size() == 3 &&
         f.frames.back().bytes[0] == ble::kFrameReadEnd);
  assert(get_u32(f.frames.back().bytes.data() + 3) == 1017);
  assert(f.frames.back().bytes[7] == ble::kErrBusy);
  f.clear();
  serve(session, read(f, handle, 1017, 4000 - 1017, ble::Link::Lan), true);
  verify_read(f, handle, 1017, 4000 - 1017, 1024, ble::Link::Lan);

  f.clear();
  auto close = request(f, ble::kOpClose, ble::Link::Lan);
  close.length = 3;
  put_u16(close.bytes + 1, handle);
  serve(session, close, true);
  assert(f.frames.size() == 1 && f.frames[0].bytes[0] == ble::kFrameClosed);
  f.clear();
  serve(session, read(f, handle, 0, 10, ble::Link::Lan), true);
  expect_error(f, ble::kErrBadHandle, ble::Link::Lan);

  f.clear();
  auto invalid = request(f, ble::kOpOpen);
  const char *name = "../secret.parquet";
  std::memcpy(invalid.bytes + 1, name, std::strlen(name));
  invalid.length += std::strlen(name);
  serve(session, invalid, true);
  expect_error(f, ble::kErrInvalidName);
  f.clear();
  f.complete = false;
  const char *valid = "sample.parquet";
  std::memcpy(invalid.bytes + 1, valid, std::strlen(valid));
  invalid.length = 1 + std::strlen(valid);
  serve(session, invalid, true);
  expect_error(f, ble::kErrNotFinalized);
  f.complete = true;
  handle = open(f, session);
  f.clear();
  f.payload[0] = 7;
  serve(session, read(f, handle, 0, 10), true);
  assert(f.frames.size() == 1 && f.frames[0].bytes[0] == ble::kFrameReadEnd &&
         f.frames[0].bytes[7] == ble::kErrBusy);
  session.close();
  assert(f.lock_depth == 0 && f.locks > 0);

  // All authenticated peers retain independent handles, even when their
  // requests are interleaved on the sole storage worker.
  // Each logical handle holds exactly one descriptor until it is released.
  f.payload[0] = 517;
  const unsigned idle_descriptors = descriptor_count();
  const auto ble_handle = open(f, session);
  assert(descriptor_count() == idle_descriptors + 1);
  std::array<std::uint16_t, ble::kMaxLanClients> lan_handles{};
  for (std::uint8_t peer = 0; peer < ble::kMaxLanClients; ++peer) {
    lan_handles[peer] =
        open(f, session, ble::Link::Lan, "sample.parquet", peer);
    assert(descriptor_count() == idle_descriptors + 2 + peer);
  }
  constexpr unsigned all_slots = aqsync::ArchiveSession::kReadHandles;
  static_assert(all_slots == 1 + ble::kMaxLanClients, "one slot per peer");
  assert(descriptor_count() == idle_descriptors + all_slots);
  for (std::uint8_t peer = 0; peer < ble::kMaxLanClients; ++peer) {
    f.clear();
    serve(session,
          read(f, lan_handles[peer], peer * 100, 2000, ble::Link::Lan, peer),
          true);
    verify_read(f, lan_handles[peer], peer * 100, 2000, 1024, ble::Link::Lan,
                peer);
    assert(descriptor_count() == idle_descriptors + all_slots);
    f.clear();
    serve(session, request(f, ble::kOpList, ble::Link::Lan, peer), true);
    assert(f.frames.size() == 2);
    for (const auto &frame : f.frames)
      assert(frame.link == ble::Link::Lan && frame.peer == peer);
  }
  f.clear();
  serve(session, read(f, ble_handle, 37, 900), true);
  verify_read(f, ble_handle, 37, 900, 512);

  // A peer cannot READ or CLOSE another peer's handle.
  f.clear();
  serve(session, read(f, lan_handles[0], 0, 10, ble::Link::Lan, 1), true);
  expect_error(f, ble::kErrBadHandle, ble::Link::Lan, 1);
  auto wrong_close = request(f, ble::kOpClose, ble::Link::Lan, 1);
  wrong_close.length = 3;
  put_u16(wrong_close.bytes + 1, lan_handles[0]);
  f.clear();
  serve(session, wrong_close, true);
  expect_error(f, ble::kErrBadHandle, ble::Link::Lan, 1);

  // OPEN and CLOSE invalidate only the requesting slot. A re-OPEN replaces the
  // slot's descriptor rather than adding one.
  const auto reopened = open(f, session, ble::Link::Lan, "sample.parquet", 0);
  assert(reopened != lan_handles[0]);
  assert(descriptor_count() == idle_descriptors + all_slots);
  auto own_close = request(f, ble::kOpClose, ble::Link::Lan, 0);
  own_close.length = 3;
  put_u16(own_close.bytes + 1, reopened);
  f.clear();
  serve(session, own_close, true);
  assert(f.frames.size() == 1 && f.frames[0].peer == 0 &&
         f.frames[0].bytes[0] == ble::kFrameClosed);
  assert(descriptor_count() == idle_descriptors + all_slots - 1);
  f.clear();
  serve(session, read(f, lan_handles[1], 91, 10, ble::Link::Lan, 1), true);
  verify_read(f, lan_handles[1], 91, 10, 1024, ble::Link::Lan, 1);

  // Stale queued OPEN/CLOSE requests cannot mutate a replacement peer.
  auto stale_open = request(f, ble::kOpOpen, ble::Link::Lan, 1);
  const char *sample = "sample.parquet";
  std::memcpy(stale_open.bytes + 1, sample, std::strlen(sample));
  stale_open.length += std::strlen(sample);
  auto stale_close = request(f, ble::kOpClose, ble::Link::Lan, 1);
  stale_close.length = 3;
  put_u16(stale_close.bytes + 1, lan_handles[1]);
  ++f.generation[slot(ble::Link::Lan, 1)];
  session.reconcile();
  assert(descriptor_count() == idle_descriptors + all_slots - 2);
  const auto replacement = open(f, session, ble::Link::Lan, sample, 1);
  assert(descriptor_count() == idle_descriptors + all_slots - 1);
  f.clear();
  serve(session, stale_open, true);
  serve(session, stale_close, true);
  assert(f.frames.empty() && f.errors.empty());
  serve(session, read(f, replacement, 5, 75, ble::Link::Lan, 1), true);
  verify_read(f, replacement, 5, 75, 1024, ble::Link::Lan, 1);
  f.clear();
  serve(session, read(f, lan_handles[2], 80, 32, ble::Link::Lan, 2), true);
  verify_read(f, lan_handles[2], 80, 32, 1024, ble::Link::Lan, 2);
  f.clear();
  serve(session, read(f, ble_handle, 90, 14), true);
  verify_read(f, ble_handle, 90, 14, 512);

  // Disconnect during a multi-frame READ stops delivery immediately, including
  // READ_END: that frame must never leak into the slot's next connection.
  f.clear();
  f.disconnect_frame = 1;
  serve(session, read(f, replacement, 0, 3000, ble::Link::Lan, 1), true);
  assert(f.frames.size() == 1 && f.frames[0].bytes[0] == ble::kFrameChunk);
  assert(f.errors.empty());
  // The next request reconciles the generation change and drops the stream.
  f.clear();
  serve(session, read(f, replacement, 0, 10, ble::Link::Lan, 1), true);
  expect_error(f, ble::kErrBadHandle, ble::Link::Lan, 1);
  assert(descriptor_count() == idle_descriptors + all_slots - 2);
  f.clear();
  serve(session, read(f, lan_handles[2], 0, 10, ble::Link::Lan, 2), true);
  verify_read(f, lan_handles[2], 0, 10, 1024, ble::Link::Lan, 2);
  session.close();
  assert(f.lock_depth == 0);
  assert(descriptor_count() == idle_descriptors);

  persistent_cases(f, session, idle_descriptors);
  remount_cases(f, session, idle_descriptors);
  abort_cases(f, session);
  session.close();
  assert(descriptor_count() == idle_descriptors);

  // File sync is read-only: the synthetic archive remains byte-for-byte equal.
  FILE *file = std::fopen((f.directory + "/sample.parquet").c_str(), "rb");
  assert(file);
  std::vector<std::uint8_t> after(f.bytes.size());
  assert(std::fread(after.data(), 1, after.size(), file) == after.size());
  assert(std::fclose(file) == 0 && after == f.bytes);
}
