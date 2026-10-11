#include "abort_signal.h"
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
  bool listing_complete = true;
  std::size_t refuse_frame = 0;
  std::size_t disconnect_frame = 0;
  // Stamp an ABORT on (abort_link, abort_peer) as this frame is delivered.
  std::size_t abort_frame = 0;
  ble::Link abort_link = ble::Link::Ble;
  std::uint8_t abort_peer = 0;
  std::vector<std::string> logs;

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
        [](void *ctx, aqsync::FileEmitter emit, void *out) {
          auto &f = *static_cast<Fixture *>(ctx);
          for (unsigned repeat = 0; repeat < f.list_repeats; ++repeat)
            for (const auto &name : f.names)
              emit(out, name.c_str(), f.bytes.size());
          return aqsync::ListResult{f.partials, f.listing_complete};
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
        [](void *ctx) { return static_cast<Fixture *>(ctx)->clock_us += 250; }};
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
  assert(session.handle(queued, true));
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
    session.handle(stamped(request(f, ble::kOpList, link)), true);
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
  session.handle(stamped(request(f, ble::kOpList)), true);
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
    session.handle(stamped(read(f, handle, 0, 8000, ble::Link::Lan, 2)), true);
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
  session.handle(queued_read, true);
  expect_aborted(ble::kOpRead, ble::Link::Lan, 2);
  assert(f.frames.empty());
  // The handle survives the ABORT; a fresh READ after it runs normally.
  f.clear();
  session.handle(stamped(read(f, handle, 10, 3000, ble::Link::Lan, 2)), true);
  verify_read(f, handle, 10, 3000, 1024, ble::Link::Lan, 2);

  // Stale marker: the stop was recorded under generation g, the marker
  // arrives for the replacement connection g+1 and names nothing.
  reset_abort();
  f.clear();
  const auto old_list = stamped(request(f, ble::kOpList, ble::Link::Lan, 1));
  stamp_abort(ble::Link::Lan, 1, f.generation[slot(ble::Link::Lan, 1)]);
  session.handle(old_list, true);
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
  session.handle(other_list, true);
  assert(f.errors.empty() && f.frames.size() == 6 &&
         f.frames.back().bytes[0] == ble::kFrameListEnd &&
         get_u32(f.frames.back().bytes.data() + 1) == 5);
  f.clear();
  f.abort_frame = 1;
  f.abort_link = ble::Link::Lan;
  f.abort_peer = 1;
  session.handle(stamped(read(f, handle, 0, 3000, ble::Link::Lan, 2)), true);
  verify_read(f, handle, 0, 3000, 1024, ble::Link::Lan, 2);
  f.clear();
  f.abort_frame = 1;
  f.abort_link = ble::Link::Ble;
  session.handle(stamped(request(f, ble::kOpList, ble::Link::Lan, 2)), true);
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
    session.handle(read(f, handle, offset, count, ble::Link::Lan, 1), true);
    verify_read(f, handle, offset, count, 1024, ble::Link::Lan, 1);
    assert(descriptor_count() == idle_descriptors + 1);
  }
  // release_files() closes the stream; a READ while the path is gone fails
  // and the handle stays valid for a retry once the path is back.
  session.release_files();
  assert(descriptor_count() == idle_descriptors);
  f.clear();
  session.handle(read(f, handle, 0, 10, ble::Link::Lan, 1), true);
  assert(f.errors.empty() && f.frames.size() == 1 &&
         f.frames[0].bytes[0] == ble::kFrameReadEnd &&
         f.frames[0].bytes[7] == ble::kErrOpenFailed);
  assert(descriptor_count() == idle_descriptors);
  assert(std::rename(moved.c_str(), path.c_str()) == 0);
  f.clear();
  session.handle(read(f, handle, 333, 1500, ble::Link::Lan, 1), true);
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
  session.handle(rejected, true);
  expect_error(f, ble::kErrNotFinalized);
  f.complete = true;
  assert(descriptor_count() == idle_descriptors + 1);
  f.clear();
  const unsigned before_missing = f.checks;
  auto missing = request(f, ble::kOpOpen);
  const char *absent = "absent.parquet";
  std::memcpy(missing.bytes + 1, absent, std::strlen(absent));
  missing.length += std::strlen(absent);
  session.handle(missing, true);
  expect_error(f, ble::kErrNotFinalized);
  assert(f.checks == before_missing);
  assert(descriptor_count() == idle_descriptors + 1);

  // A full descriptor table is OpenFailed, not NotFinalized, and the open
  // LAN handle keeps reading through its existing descriptor.
  {
    DescriptorLimit limit;
    f.clear();
    session.handle(rejected, true);
    expect_error(f, ble::kErrOpenFailed);
    assert(f.logs.size() == 1 && f.logs[0].find("BLE OPEN FAIL errno=") == 0);
    f.clear();
    session.handle(read(f, lan, 4000, 2000, ble::Link::Lan, 1), true);
    verify_read(f, lan, 4000, 2000, 1024, ble::Link::Lan, 1);
  }
  session.close();
  assert(descriptor_count() == idle_descriptors);
}
} // namespace

int main(int argc, char **argv) {
  assert(argc == 2);
  Fixture f(argv[1]);
  aqsync::ArchiveSession session(f.archive(), f.transport());

  assert(!session.handle(request(f, ble::kOpStatus), true));
  assert(session.handle(request(f, ble::kOpList), false));
  expect_error(f, ble::kErrStorage);
  f.clear();
  assert(session.handle(request(f, ble::kOpList), true));
  assert(f.frames.size() == 2 && f.listed == 1 && f.progress == 2);
  const auto &list = f.frames.back().bytes;
  assert(list.size() == 15 && list[0] == ble::kFrameListEnd);
  assert(get_u32(list.data() + 1) == 1 && get_u16(list.data() + 5) == 65535);
  assert(get_u32(list.data() + 7) == 123 && get_u32(list.data() + 11) == 45);
  f.clear();
  f.refuse_frame = 1;
  session.handle(request(f, ble::kOpList), true);
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
  session.handle(request(f, ble::kOpList), true);
  expect_error(f, ble::kErrBusy);
  assert(f.listed == 1); // no false empty listing or cleared new-files hint
  f.payload[0] = 517;

  // SD failure after FILE frames is an error, never a successful prefix.
  for (const auto link : {ble::Link::Ble, ble::Link::Lan}) {
    f.clear();
    f.listing_complete = false;
    session.handle(request(f, ble::kOpList, link), true);
    assert(f.frames.size() == 1 && f.frames[0].bytes[0] == ble::kFrameFile);
    assert(f.errors.size() == 1 && f.errors[0].op == ble::kOpList &&
           f.errors[0].code == ble::kErrStorage && f.errors[0].link == link);
    assert(f.listed == 1);
  }
  f.listing_complete = true;
  f.clear();
  f.refuse_frame = 2; // LIST_END delivery fails after a successful traversal.
  session.handle(request(f, ble::kOpList), true);
  assert(f.frames.size() == 2 && f.listed == 1);
  f.clear();
  session.handle(request(f, ble::kOpList, ble::Link::Lan), true);
  assert(f.frames.size() == 2 && f.listed == 2 && f.errors.empty());
  f.clear();
  f.list_repeats = 70000; // Proto 3 LIST_END count is u32, past the old cap.
  session.handle(request(f, ble::kOpList, ble::Link::Lan), true);
  assert(f.frames.size() == 70001 && f.listed == 3 && f.errors.empty());
  assert(f.frames.back().bytes[0] == ble::kFrameListEnd &&
         f.frames.back().bytes.size() == 15 &&
         get_u32(f.frames.back().bytes.data() + 1) == 70000);
  f.list_repeats = 1;

  auto handle = open(f, session);
  f.clear();
  f.payload[0] = 24; // LIST fits, but OPENED has a larger header
  auto too_small = request(f, ble::kOpOpen);
  const char *file_name = "sample.parquet";
  std::memcpy(too_small.bytes + 1, file_name, std::strlen(file_name));
  too_small.length += std::strlen(file_name);
  session.handle(too_small, true);
  expect_error(f, ble::kErrBusy);
  f.payload[0] = 517;
  f.clear();
  session.handle(read(f, handle, 0, 10), true);
  verify_read(f, handle, 0, 10, 512); // rejected OPEN preserves prior handle
  f.clear();
  session.handle(read(f, handle, 0, UINT32_MAX), true);
  verify_read(f, handle, 0, ble::kMaxRead, 512);
  f.clear();
  session.handle(read(f, handle, f.bytes.size() - 15, 200), true);
  verify_read(f, handle, f.bytes.size() - 15, 15, 512);
  f.clear();
  session.handle(read(f, handle, f.bytes.size() + 1, 1), true);
  expect_error(f, ble::kErrRange);
  f.clear();
  session.handle(read(f, handle, 0, 10, ble::Link::Lan), true);
  expect_error(f, ble::kErrBadHandle, ble::Link::Lan);
  ++f.generation[1]; // another transport reconnect cannot kill a BLE handle
  f.clear();
  session.handle(read(f, handle, 7, 22), true);
  verify_read(f, handle, 7, 22, 512);
  ++f.generation[0];
  f.clear();
  session.handle(read(f, handle, 0, 10), true);
  expect_error(f, ble::kErrBadHandle);

  handle = open(f, session, ble::Link::Lan);
  f.clear();
  session.handle(read(f, handle, 123, 2040, ble::Link::Lan), true);
  verify_read(f, handle, 123, 2040, 1024, ble::Link::Lan);
  f.clear();
  auto malformed = read(f, handle, 0, 10, ble::Link::Lan);
  --malformed.length;
  session.handle(malformed, true);
  expect_error(f, ble::kErrMalformed, ble::Link::Lan);
  f.clear();
  f.refuse_frame = 2;
  session.handle(read(f, handle, 0, 4000, ble::Link::Lan), true);
  assert(f.frames.size() == 3 &&
         f.frames.back().bytes[0] == ble::kFrameReadEnd);
  assert(get_u32(f.frames.back().bytes.data() + 3) == 1017);
  assert(f.frames.back().bytes[7] == ble::kErrBusy);
  f.clear();
  session.handle(read(f, handle, 1017, 4000 - 1017, ble::Link::Lan), true);
  verify_read(f, handle, 1017, 4000 - 1017, 1024, ble::Link::Lan);

  f.clear();
  auto close = request(f, ble::kOpClose, ble::Link::Lan);
  close.length = 3;
  put_u16(close.bytes + 1, handle);
  session.handle(close, true);
  assert(f.frames.size() == 1 && f.frames[0].bytes[0] == ble::kFrameClosed);
  f.clear();
  session.handle(read(f, handle, 0, 10, ble::Link::Lan), true);
  expect_error(f, ble::kErrBadHandle, ble::Link::Lan);

  f.clear();
  auto invalid = request(f, ble::kOpOpen);
  const char *name = "../secret.parquet";
  std::memcpy(invalid.bytes + 1, name, std::strlen(name));
  invalid.length += std::strlen(name);
  session.handle(invalid, true);
  expect_error(f, ble::kErrInvalidName);
  f.clear();
  f.complete = false;
  const char *valid = "sample.parquet";
  std::memcpy(invalid.bytes + 1, valid, std::strlen(valid));
  invalid.length = 1 + std::strlen(valid);
  session.handle(invalid, true);
  expect_error(f, ble::kErrNotFinalized);
  f.complete = true;
  handle = open(f, session);
  f.clear();
  f.payload[0] = 7;
  session.handle(read(f, handle, 0, 10), true);
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
    session.handle(
        read(f, lan_handles[peer], peer * 100, 2000, ble::Link::Lan, peer),
        true);
    verify_read(f, lan_handles[peer], peer * 100, 2000, 1024, ble::Link::Lan,
                peer);
    assert(descriptor_count() == idle_descriptors + all_slots);
    f.clear();
    session.handle(request(f, ble::kOpList, ble::Link::Lan, peer), true);
    assert(f.frames.size() == 2);
    for (const auto &frame : f.frames)
      assert(frame.link == ble::Link::Lan && frame.peer == peer);
  }
  f.clear();
  session.handle(read(f, ble_handle, 37, 900), true);
  verify_read(f, ble_handle, 37, 900, 512);

  // A peer cannot READ or CLOSE another peer's handle.
  f.clear();
  session.handle(read(f, lan_handles[0], 0, 10, ble::Link::Lan, 1), true);
  expect_error(f, ble::kErrBadHandle, ble::Link::Lan, 1);
  auto wrong_close = request(f, ble::kOpClose, ble::Link::Lan, 1);
  wrong_close.length = 3;
  put_u16(wrong_close.bytes + 1, lan_handles[0]);
  f.clear();
  session.handle(wrong_close, true);
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
  session.handle(own_close, true);
  assert(f.frames.size() == 1 && f.frames[0].peer == 0 &&
         f.frames[0].bytes[0] == ble::kFrameClosed);
  assert(descriptor_count() == idle_descriptors + all_slots - 1);
  f.clear();
  session.handle(read(f, lan_handles[1], 91, 10, ble::Link::Lan, 1), true);
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
  session.handle(stale_open, true);
  session.handle(stale_close, true);
  assert(f.frames.empty() && f.errors.empty());
  session.handle(read(f, replacement, 5, 75, ble::Link::Lan, 1), true);
  verify_read(f, replacement, 5, 75, 1024, ble::Link::Lan, 1);
  f.clear();
  session.handle(read(f, lan_handles[2], 80, 32, ble::Link::Lan, 2), true);
  verify_read(f, lan_handles[2], 80, 32, 1024, ble::Link::Lan, 2);
  f.clear();
  session.handle(read(f, ble_handle, 90, 14), true);
  verify_read(f, ble_handle, 90, 14, 512);

  // Disconnect during a multi-frame READ stops delivery immediately, including
  // READ_END: that frame must never leak into the slot's next connection.
  f.clear();
  f.disconnect_frame = 1;
  session.handle(read(f, replacement, 0, 3000, ble::Link::Lan, 1), true);
  assert(f.frames.size() == 1 && f.frames[0].bytes[0] == ble::kFrameChunk);
  assert(f.errors.empty());
  // The next request reconciles the generation change and drops the stream.
  f.clear();
  session.handle(read(f, replacement, 0, 10, ble::Link::Lan, 1), true);
  expect_error(f, ble::kErrBadHandle, ble::Link::Lan, 1);
  assert(descriptor_count() == idle_descriptors + all_slots - 2);
  f.clear();
  session.handle(read(f, lan_handles[2], 0, 10, ble::Link::Lan, 2), true);
  verify_read(f, lan_handles[2], 0, 10, 1024, ble::Link::Lan, 2);
  session.close();
  assert(f.lock_depth == 0);
  assert(descriptor_count() == idle_descriptors);

  persistent_cases(f, session, idle_descriptors);
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
