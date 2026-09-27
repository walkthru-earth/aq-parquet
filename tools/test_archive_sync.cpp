#include "archive_sync.h"
#include "sync_codec.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
using namespace aq::sync;
struct Frame {
  ble::Link link;
  std::vector<std::uint8_t> bytes;
};
struct Error {
  ble::Link link;
  ble::Op op;
  ble::Error code;
};
struct Fixture {
  std::string directory;
  std::vector<std::uint8_t> bytes;
  std::vector<Frame> frames;
  std::vector<Error> errors;
  std::array<std::uint32_t, 2> generation{1, 1};
  std::array<std::uint16_t, 2> payload{517, 1024};
  std::vector<std::string> names{"sample.parquet"};
  unsigned partials = 70000;
  unsigned listed = 0, progress = 0, locks = 0, lock_depth = 0;
  bool complete = true;
  std::size_t refuse_frame = 0;

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
  }
  aqsync::ArchiveHooks archive() {
    return {
        this,
        [](void *ctx, aqsync::FileEmitter emit, void *out) {
          auto &f = *static_cast<Fixture *>(ctx);
          for (const auto &name : f.names)
            emit(out, name.c_str(), f.bytes.size());
          return f.partials;
        },
        [](void *ctx, const char *name, char *path, std::size_t size) {
          auto &f = *static_cast<Fixture *>(ctx);
          if (!safe_parquet_name(name))
            return false;
          const int n =
              std::snprintf(path, size, "%s/%s", f.directory.c_str(), name);
          return n > 0 && static_cast<std::size_t>(n) < size;
        },
        [](void *ctx, const char *, std::uint32_t &size, std::uint32_t &crc) {
          auto &f = *static_cast<Fixture *>(ctx);
          size = f.bytes.size();
          crc = crc_update(0xffffffffU, f.bytes.data(), size) ^ 0xffffffffU;
          return f.complete &&
                 parquet_complete(size, f.bytes.data(),
                                  f.bytes.data() + f.bytes.size() - 8);
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
        nullptr};
  }
  aqsync::ArchiveTransport transport() {
    return {this,
            [](void *ctx, ble::Link link) {
              return static_cast<Fixture *>(ctx)->generation[unsigned(link)];
            },
            [](void *ctx, ble::Link link) {
              return static_cast<Fixture *>(ctx)->payload[unsigned(link)];
            },
            [](void *ctx, ble::Link link, const std::uint8_t *data,
               std::size_t size) {
              auto &f = *static_cast<Fixture *>(ctx);
              f.frames.push_back({link, {data, data + size}});
              return !f.refuse_frame || f.frames.size() != f.refuse_frame;
            },
            [](void *ctx, ble::Link link, ble::Op op, ble::Error error,
               const char *) {
              static_cast<Fixture *>(ctx)->errors.push_back({link, op, error});
              return true;
            },
            [](void *ctx) { ++static_cast<Fixture *>(ctx)->progress; }};
  }
};
ble::ControlRequest request(ble::Op op, ble::Link link = ble::Link::Ble) {
  ble::ControlRequest r;
  r.link = link;
  r.bytes[0] = op;
  r.length = 1;
  return r;
}
std::uint16_t open(Fixture &f, aqsync::ArchiveSession &session,
                   ble::Link link = ble::Link::Ble,
                   const char *name = "sample.parquet") {
  f.clear();
  auto r = request(ble::kOpOpen, link);
  std::memcpy(r.bytes + 1, name, std::strlen(name));
  r.length += std::strlen(name);
  assert(session.handle(r, true));
  assert(f.errors.empty());
  assert(f.frames.size() == 1 && f.frames[0].link == link);
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
ble::ControlRequest read(std::uint16_t handle, std::uint32_t offset,
                         std::uint32_t count, ble::Link link = ble::Link::Ble) {
  auto r = request(ble::kOpRead, link);
  r.length = 11;
  put_u16(r.bytes + 1, handle);
  put_u32(r.bytes + 3, offset);
  put_u32(r.bytes + 7, count);
  return r;
}
void expect_error(const Fixture &f, ble::Error code,
                  ble::Link link = ble::Link::Ble) {
  assert(f.errors.size() == 1 && f.errors.back().code == code &&
         f.errors.back().link == link);
  assert(f.frames.empty());
}
void verify_read(Fixture &f, std::uint16_t handle, std::uint32_t offset,
                 std::uint32_t count, std::size_t max_frame,
                 ble::Link link = ble::Link::Ble) {
  assert(f.errors.empty());
  std::vector<std::uint8_t> received;
  for (std::size_t i = 0; i + 1 < f.frames.size(); ++i) {
    const auto &frame = f.frames[i];
    assert(frame.link == link && frame.bytes.size() <= max_frame);
    assert(frame.bytes[0] == ble::kFrameChunk);
    assert(get_u16(frame.bytes.data() + 1) == handle);
    assert(get_u32(frame.bytes.data() + 3) == offset + received.size());
    received.insert(received.end(), frame.bytes.begin() + 7, frame.bytes.end());
  }
  assert(received.size() == count);
  assert(std::memcmp(received.data(), f.bytes.data() + offset, count) == 0);
  const auto &end = f.frames.back();
  assert(end.link == link && end.bytes.size() == 8 &&
         end.bytes[0] == ble::kFrameReadEnd && end.bytes[7] == 0);
  assert(get_u16(end.bytes.data() + 1) == handle);
  assert(get_u32(end.bytes.data() + 3) == offset + count);
}
} // namespace

int main(int argc, char **argv) {
  assert(argc == 2);
  Fixture f(argv[1]);
  aqsync::ArchiveSession session(f.archive(), f.transport());

  assert(!session.handle(request(ble::kOpStatus), true));
  assert(session.handle(request(ble::kOpList), false));
  expect_error(f, ble::kErrStorage);
  f.clear();
  assert(session.handle(request(ble::kOpList), true));
  assert(f.frames.size() == 2 && f.listed == 1 && f.progress == 2);
  const auto &list = f.frames.back().bytes;
  assert(list.size() == 13 && list[0] == ble::kFrameListEnd);
  assert(get_u16(list.data() + 1) == 1 && get_u16(list.data() + 3) == 65535);
  assert(get_u32(list.data() + 5) == 123 && get_u32(list.data() + 9) == 45);
  f.clear();
  f.refuse_frame = 1;
  session.handle(request(ble::kOpList), true);
  assert(f.frames.size() == 1 && f.listed == 1); // no LIST_END after refusal
  f.clear();
  f.payload[0] = 18;
  session.handle(request(ble::kOpList), true);
  assert(f.frames.size() == 1 &&
         get_u16(f.frames.back().bytes.data() + 1) == 0);
  f.payload[0] = 517;

  auto handle = open(f, session);
  f.clear();
  session.handle(read(handle, 0, UINT32_MAX), true);
  verify_read(f, handle, 0, ble::kMaxRead, 512);
  f.clear();
  session.handle(read(handle, f.bytes.size() - 15, 200), true);
  verify_read(f, handle, f.bytes.size() - 15, 15, 512);
  f.clear();
  session.handle(read(handle, f.bytes.size() + 1, 1), true);
  expect_error(f, ble::kErrRange);
  f.clear();
  session.handle(read(handle, 0, 10, ble::Link::Lan), true);
  expect_error(f, ble::kErrBadHandle, ble::Link::Lan);
  ++f.generation[1]; // another transport reconnect cannot kill a BLE handle
  f.clear();
  session.handle(read(handle, 7, 22), true);
  verify_read(f, handle, 7, 22, 512);
  ++f.generation[0];
  f.clear();
  session.handle(read(handle, 0, 10), true);
  expect_error(f, ble::kErrBadHandle);

  handle = open(f, session, ble::Link::Lan);
  f.clear();
  session.handle(read(handle, 123, 2040, ble::Link::Lan), true);
  verify_read(f, handle, 123, 2040, 1024, ble::Link::Lan);
  f.clear();
  auto malformed = read(handle, 0, 10, ble::Link::Lan);
  --malformed.length;
  session.handle(malformed, true);
  expect_error(f, ble::kErrMalformed, ble::Link::Lan);
  f.clear();
  f.refuse_frame = 2;
  session.handle(read(handle, 0, 4000, ble::Link::Lan), true);
  assert(f.frames.size() == 3 &&
         f.frames.back().bytes[0] == ble::kFrameReadEnd);
  assert(get_u32(f.frames.back().bytes.data() + 3) == 1017);
  assert(f.frames.back().bytes[7] == ble::kErrBusy);
  f.clear();
  session.handle(read(handle, 1017, 4000 - 1017, ble::Link::Lan), true);
  verify_read(f, handle, 1017, 4000 - 1017, 1024, ble::Link::Lan);

  f.clear();
  auto close = request(ble::kOpClose, ble::Link::Lan);
  close.length = 3;
  put_u16(close.bytes + 1, handle);
  session.handle(close, true);
  assert(f.frames.size() == 1 && f.frames[0].bytes[0] == ble::kFrameClosed);
  f.clear();
  session.handle(read(handle, 0, 10, ble::Link::Lan), true);
  expect_error(f, ble::kErrBadHandle, ble::Link::Lan);

  f.clear();
  auto invalid = request(ble::kOpOpen);
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
  session.handle(read(handle, 0, 10), true);
  assert(f.frames.size() == 1 && f.frames[0].bytes[0] == ble::kFrameReadEnd &&
         f.frames[0].bytes[7] == ble::kErrBusy);
  session.close();
  assert(f.lock_depth == 0 && f.locks > 0);

  // File sync is read-only: the synthetic archive remains byte-for-byte equal.
  FILE *file = std::fopen((f.directory + "/sample.parquet").c_str(), "rb");
  assert(file);
  std::vector<std::uint8_t> after(f.bytes.size());
  assert(std::fread(after.data(), 1, after.size(), file) == after.size());
  assert(std::fclose(file) == 0 && after == f.bytes);
}
