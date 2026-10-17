#include "../firmware/esp-idf-waveshare-sim7670g/logger/pms5003t_identity.h"
#include <abort_signal.h>
#include <archive_sync.h>
#include <control_sync.h>
#include <debug_log.h>
#include <device_config.h>
#include <disconnect.h>
#include <sync_codec.h>
#include <wifi_link.h>

#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// The default board layout (runtime Kconfig), 64 KiB and 64 events.
constexpr DebugLog::Layout kTestLayout{65536, 64};

namespace {
unsigned bonds_cleared = 0, sessions_dropped = 0, wifi_reapplied = 0,
         time_reapplied = 0;
struct Reply {
  ble::Link link;
  std::vector<std::uint8_t> bytes;
};
struct Failure {
  ble::Link link;
  ble::Op op;
  ble::Error code;
};
// abort::sequences and abort::stopped are process-wide; reset between cases.
void reset_abort() {
  for (auto &sequence : aqsync::abort::sequences)
    sequence.store(0);
  for (auto &entry : aqsync::abort::stopped)
    entry = aqsync::abort::Stopped{};
}
// What a transport task does with a request the queue accepted.
ble::ControlRequest stamped(ble::ControlRequest request) {
  aqsync::abort::stamp(request);
  aqsync::abort::commit(request);
  return request;
}
struct Peer {
  std::uint16_t payload = 600;
  unsigned attempts = 0;
  unsigned fail_attempt = 0;
  // Stamp an ABORT on this link while this respond attempt is in flight.
  unsigned abort_attempt = 0;
  ble::Link abort_link = ble::Link::Ble;
  std::vector<Reply> successful;
  std::vector<Failure> errors;

  aqsync::ControlReplies callbacks() {
    return {this, payload_max, respond, error};
  }
  static std::uint16_t payload_max(void *context, ble::Link) {
    return static_cast<Peer *>(context)->payload;
  }
  static bool respond(void *context, ble::Link link, const std::uint8_t *bytes,
                      std::size_t size) {
    auto &peer = *static_cast<Peer *>(context);
    if (++peer.attempts == peer.abort_attempt) {
      ble::ControlRequest marker;
      marker.link = peer.abort_link;
      marker.bytes[0] = ble::kOpAbort;
      marker.length = 1;
      aqsync::abort::stamp(marker);
      aqsync::abort::commit(marker);
    }
    if (peer.attempts == peer.fail_attempt)
      return false;
    peer.successful.push_back({link, {bytes, bytes + size}});
    return true;
  }
  static bool error(void *context, ble::Link link, ble::Op op, ble::Error code,
                    const char *) {
    static_cast<Peer *>(context)->errors.push_back({link, op, code});
    return true;
  }
};

ble::ControlRequest request(ble::Op op, ble::Link link = ble::Link::Ble,
                            const std::string &body = {}) {
  ble::ControlRequest request;
  assert(body.size() + 1 <= ble::kMaxControlBytes);
  request.bytes[0] = op;
  std::memcpy(request.bytes + 1, body.data(), body.size());
  request.length = static_cast<std::uint16_t>(body.size() + 1);
  request.link = link;
  return request;
}
void handle(const ble::ControlRequest &request, Peer &peer) {
  assert(aqsync::handle_common_control(request, peer.callbacks()));
}

void paged_configuration() {
  for (std::uint8_t page = 0; page <= 4; ++page) {
    Peer peer;
    handle(
        request(ble::kOpGetConfig, ble::Link::Lan, std::string(1, char(page))),
        peer);
    assert(peer.errors.empty() && peer.successful.size() == 1);
    const auto &frame = peer.successful[0].bytes;
    assert(frame[0] == ble::kFrameConfig && frame.size() < 482);
    const std::string json(frame.begin() + 2, frame.end());
    assert(json.find(page == 0   ? "\"ble\""
                     : page == 1 ? "\"location\""
                                 : "\"ntp\"") != std::string::npos);
  }
  for (const auto &body : {std::string(1, char(5)), std::string(2, char(0))}) {
    Peer peer;
    handle(request(ble::kOpGetConfig, ble::Link::Ble, body), peer);
    assert(peer.successful.empty() && peer.errors.size() == 1);
    assert(peer.errors[0].code == ble::kErrMalformed);
  }
  const unsigned previous_wifi = wifi_reapplied, previous_time = time_reapplied;
  Peer peer;
  handle(request(ble::kOpSetConfig, ble::Link::Lan, "ntp.interval_s=7200"),
         peer);
  assert(peer.errors.empty() && time_reapplied == previous_time + 1);
  assert(wifi_reapplied == previous_wifi);
  peer = Peer{};
  handle(request(ble::kOpSetConfig, ble::Link::Lan, "location.country=EG"),
         peer);
  assert(peer.errors.empty() && time_reapplied == previous_time + 1);
  assert(wifi_reapplied == previous_wifi);
}

void configuration() {
  config::set_sensor_policy({pms5003t::kVendor, pms5003t::kModel,
                             pms5003t::valid_serial, pms5003t::batch_eligible});
  assert(config::load(false));
  Peer peer;
  handle(request(ble::kOpGetConfig, ble::Link::Lan), peer);
  assert(peer.successful.size() == 1 && peer.errors.empty());
  const auto &frame = peer.successful[0];
  assert(frame.link == ble::Link::Lan && frame.bytes[0] == ble::kFrameConfig);
  assert(frame.bytes[1] == 0);
  const std::string json(frame.bytes.begin() + 2, frame.bytes.end());
  assert(json.find("\"pin\":") == std::string::npos);
  assert(json.find("\"psk\":") == std::string::npos);
  assert(json.find("\"token\":") == std::string::npos);
  assert(json.find("\"bonds\":3") != std::string::npos);
  assert(json.find("\"clients\":3") != std::string::npos);
  const auto before = config::get();
  peer = Peer{};
  handle(request(ble::kOpSetConfig, ble::Link::Ble,
                 "ble.clear_bonds=1\nlan.rotate_token=1\nwifi.ssid=fixture"
                 "\nwifi.on=1\nble.pin=000001"),
         peer);
  assert(bonds_cleared == 1 && sessions_dropped == 1 && wifi_reapplied == 1);
  assert(peer.successful.size() == 1 && peer.errors.empty());
  assert(peer.successful[0].bytes[0] == ble::kFrameConfig);
  assert(peer.successful[0].bytes[1] == 1); // reboot required for pairing PIN
  assert(config::get().wifi_on && config::get().pin == 1);
  assert(std::memcmp(before.token, config::get().token, config::kTokenBytes) !=
         0);
  peer = Peer{};
  handle(request(ble::kOpSetConfig, ble::Link::Lan,
                 "ble.clear_bonds=1\nlan.rotate_token=1\nwifi.on=bad"),
         peer);
  assert(peer.successful.empty() && peer.errors.size() == 1);
  assert(peer.errors[0].link == ble::Link::Lan &&
         peer.errors[0].code == ble::kErrInvalidConfig);
  assert(bonds_cleared == 1 && sessions_dropped == 1 && wifi_reapplied == 1);
}

void config_chunks() {
  config::Actions actions;
  char bad_key[48];
  const std::string profile =
      "sensor.vendor=Plantower\nsensor.model=PMS5003T\n"
      "sensor.serial=PMS5003T-202604081332\nsensor.batch_candidate=1";
  assert(config::apply_lines(profile.data(), profile.size(), bad_key,
                             sizeof(bad_key), actions));
  Peer large;
  std::string expected;
  // Choose a valid UTF-8 SSID that crosses a 128-byte CONFIG_CHUNK boundary.
  for (std::size_t prefix = 0; prefix <= 30; ++prefix) {
    const std::string setting =
        "wifi.ssid=" + std::string(prefix, 'x') + "\xC3\xA9";
    assert(config::apply_lines(setting.data(), setting.size(), bad_key,
                               sizeof(bad_key), actions));
    large = Peer{};
    handle(request(ble::kOpGetConfig, ble::Link::Lan), large);
    assert(large.successful.size() == 1 &&
           large.successful[0].bytes[0] == ble::kFrameConfig);
    expected.assign(large.successful[0].bytes.begin() + 2,
                    large.successful[0].bytes.end());
    const auto at = expected.find("\xC3\xA9");
    if (at != std::string::npos && at % 128 == 127)
      break;
  }
  const auto unicode_at = expected.find("\xC3\xA9");
  assert(unicode_at != std::string::npos && unicode_at % 128 == 127);

  Peer small;
  small.payload = 244; // MTU 247: CONFIG cannot fit one ATT notification.
  handle(request(ble::kOpGetConfig), small);
  assert(small.errors.empty() && small.successful.size() >= 3);
  std::string actual;
  for (const auto &reply : small.successful) {
    const auto &bytes = reply.bytes;
    assert(reply.link == ble::Link::Ble && bytes[0] == ble::kFrameConfigChunk &&
           bytes.size() <= 244);
    assert(bytes[1] == large.successful[0].bytes[1] &&
           aq::sync::get_u16(bytes.data() + 2) == expected.size() &&
           aq::sync::get_u16(bytes.data() + 4) == actual.size());
    actual.append(bytes.begin() + 6, bytes.end());
  }
  assert(actual == expected);

  Peer failed;
  failed.payload = 244;
  failed.fail_attempt = 2;
  handle(request(ble::kOpGetConfig), failed);
  assert(failed.successful.size() == 1 && failed.attempts == 2);

  Peer full;
  full.payload = 514; // MTU 517 carries legacy single-frame CONFIG.
  handle(request(ble::kOpGetConfig), full);
  assert(full.successful.size() == 1 &&
         full.successful[0].bytes[0] == ble::kFrameConfig);
}

void token_guard() {
  Peer peer;
  handle(request(ble::kOpGetToken, ble::Link::Lan), peer);
  assert(peer.successful.empty() && peer.errors.size() == 1);
  assert(peer.errors[0].code == ble::kErrNotOnThisLink);
  assert(peer.errors[0].link == ble::Link::Lan);
  peer = Peer{};
  handle(request(ble::kOpGetToken, ble::Link::Ble), peer);
  assert(peer.errors.empty() && peer.successful.size() == 1);
  const auto &frame = peer.successful[0];
  assert(frame.link == ble::Link::Ble && frame.bytes[0] == ble::kFrameToken);
  assert(frame.bytes.size() == 3 + config::kTokenBytes);
  assert(aq::sync::get_u16(frame.bytes.data() + 1) == config::kLanPort);
  assert(std::memcmp(frame.bytes.data() + 3, config::get().token,
                     config::kTokenBytes) == 0);
}

void logs() {
  // Attach a persistent block, as a PSRAM board does, so the ring is 64 KiB.
  static std::vector<std::uint32_t> block(
      DebugLog::persistent_bytes(kTestLayout) / 4 + 1);
  assert(!aqlog.attach(block.data(), block.size() * 4, kTestLayout, true));
  assert(aqlog.ring_bytes() == 65536);
  const std::string contents(DebugLog::kFallbackRingBytes, 'L');
  assert(aqlog.write(reinterpret_cast<const std::uint8_t *>(contents.data()),
                     contents.size()) == contents.size());
  std::string length(2, '\0');
  aq::sync::put_u16(reinterpret_cast<std::uint8_t *>(length.data()), 1500);
  for (const auto link : {ble::Link::Ble, ble::Link::Lan}) {
    Peer peer;
    peer.payload = 2000; // cap BLE at 512, LAN at 1024
    handle(request(ble::kOpLogTail, link, length), peer);
    const auto cap = link == ble::Link::Ble ? 512U : 1024U;
    assert(peer.errors.empty());
    std::size_t count = 0;
    for (std::size_t i = 0; i + 1 < peer.successful.size(); ++i) {
      const auto &frame = peer.successful[i];
      assert(frame.link == link && frame.bytes[0] == ble::kFrameLog);
      assert(frame.bytes.size() <= cap);
      for (std::size_t byte = 1; byte < frame.bytes.size(); ++byte)
        assert(frame.bytes[byte] == 'L');
      count += frame.bytes.size() - 1;
    }
    const auto &end = peer.successful.back().bytes;
    assert(end.size() == 7 && end[0] == ble::kFrameLogEnd);
    assert(aq::sync::get_u16(end.data() + 5) == count && count == 1500);
    assert(aq::sync::get_u32(end.data() + 1) >= DebugLog::kFallbackRingBytes);
  }
  // A full 65535-byte tail streams from the 64 KiB ring in slices, in order.
  std::string sequence(70000, '\0');
  for (std::size_t i = 0; i < sequence.size(); ++i)
    sequence[i] = static_cast<char>('A' + (i * 13) % 26);
  assert(aqlog.write(reinterpret_cast<const std::uint8_t *>(sequence.data()),
                     sequence.size()) == sequence.size());
  aq::sync::put_u16(reinterpret_cast<std::uint8_t *>(length.data()), 65535);
  for (const auto link : {ble::Link::Ble, ble::Link::Lan}) {
    Peer peer;
    peer.payload = 2000;
    handle(request(ble::kOpLogTail, link, length), peer);
    const auto cap = link == ble::Link::Ble ? 512U : 1024U;
    std::string streamed;
    for (std::size_t i = 0; i + 1 < peer.successful.size(); ++i) {
      const auto &frame = peer.successful[i].bytes;
      assert(frame[0] == ble::kFrameLog && frame.size() <= cap);
      streamed.append(frame.begin() + 1, frame.end());
    }
    assert(peer.successful.size() - 1 == (65535 + cap - 2) / (cap - 1));
    assert(streamed == sequence.substr(sequence.size() - 65535));
    const auto &end = peer.successful.back().bytes;
    assert(end.size() == 7 && aq::sync::get_u16(end.data() + 5) == 65535);
  }
  Peer failed;
  failed.fail_attempt = 2;
  handle(request(ble::kOpLogTail, ble::Link::Ble, length), failed);
  assert(failed.successful.size() ==
         2); // first slice + LOG_END, no further data
  const auto &end = failed.successful.back().bytes;
  assert(end[0] == ble::kFrameLogEnd);
  assert(aq::sync::get_u16(end.data() + 5) == 511);
  Peer malformed;
  handle(request(ble::kOpLogTail, ble::Link::Lan, "x"), malformed);
  assert(malformed.successful.empty() && malformed.errors.size() == 1);
  assert(malformed.errors[0].code == ble::kErrMalformed);
}

void diagnostics() {
  // GET_DIAG: newest events that fit the link, oldest first.
  for (unsigned i = 0; i < 70; ++i)
    ble::record_disconnect(ble::DisconnectReason::IdleTimeout,
                           static_cast<std::uint8_t>(1 + i % 3), i, 0, 0,
                           std::int64_t(i) * 1000000);
  for (const auto link : {ble::Link::Ble, ble::Link::Lan}) {
    Peer peer;
    peer.payload = 2000;
    handle(request(ble::kOpGetDiag, link), peer);
    assert(peer.errors.empty() && peer.successful.size() == 1);
    const auto &frame = peer.successful[0].bytes;
    const std::size_t n = link == ble::Link::Ble ? 29 : 61;
    assert(frame[0] == ble::kFrameDiag && frame[37] == n);
    assert(frame.size() == 38 + 16 * n);
    assert(aq::sync::get_u16(frame.data() + 1) == aqlog.boot_seq());
    assert(aq::sync::get_u16(frame.data() + 3 + 2 * 1) == 70);
    const auto last = ble::decode_event(frame.data() + 38 + 16 * (n - 1));
    const auto first = ble::decode_event(frame.data() + 38);
    assert(last.generation == 69 && first.generation == 70 - n);
    assert(last.uptime_s == 69 && last.reason == 1);
  }
  Peer narrow;
  narrow.payload = 37;
  handle(request(ble::kOpGetDiag, ble::Link::Ble), narrow);
  assert(narrow.successful.empty() && narrow.errors.size() == 1);
  assert(narrow.errors[0].code == ble::kErrBusy);
  Peer malformed;
  handle(request(ble::kOpGetDiag, ble::Link::Lan, "x"), malformed);
  assert(malformed.errors.size() == 1 &&
         malformed.errors[0].code == ble::kErrMalformed);

  // ABORT while idle: ABORTED op 0.
  Peer idle;
  handle(request(ble::kOpAbort, ble::Link::Lan), idle);
  assert(idle.errors.empty() && idle.successful.size() == 1);
  assert((idle.successful[0].bytes ==
          std::vector<std::uint8_t>{ble::kFrameAborted, 0}));
  Peer bad_abort;
  handle(request(ble::kOpAbort, ble::Link::Ble, "x"), bad_abort);
  assert(bad_abort.errors.size() == 1 &&
         bad_abort.errors[0].code == ble::kErrMalformed);
}

bool aborted_reply(const Peer &peer, std::uint8_t op) {
  return peer.errors.empty() && peer.successful.size() == 1 &&
         peer.successful[0].bytes ==
             std::vector<std::uint8_t>{ble::kFrameAborted, op};
}

// ABORT through the worker: the stopped op ends with ERROR aborted and the
// queued ABORT marker answers ABORTED naming it exactly once.
void aborts() {
  std::string length(2, '\0');
  aq::sync::put_u16(reinterpret_cast<std::uint8_t *>(length.data()), 65535);

  // LOG_TAIL aborted mid stream, after a delivered slice and on a refused
  // one: ERROR aborted, no LOG_END, then ABORTED names LOG_TAIL.
  for (const unsigned refuse : {0U, 3U}) {
    reset_abort();
    Peer peer;
    peer.payload = 2000;
    peer.abort_attempt = 3;
    peer.fail_attempt = refuse;
    peer.abort_link = ble::Link::Lan;
    handle(stamped(request(ble::kOpLogTail, ble::Link::Lan, length)), peer);
    assert(peer.successful.size() == (refuse ? 2U : 3U));
    for (const auto &reply : peer.successful)
      assert(reply.bytes[0] == ble::kFrameLog);
    assert(peer.errors.size() == 1 && peer.errors[0].op == ble::kOpLogTail &&
           peer.errors[0].code == ble::kErrAborted &&
           peer.errors[0].link == ble::Link::Lan);
    Peer marker;
    handle(stamped(request(ble::kOpAbort, ble::Link::Lan)), marker);
    assert(aborted_reply(marker, ble::kOpLogTail));
    Peer again;
    handle(stamped(request(ble::kOpAbort, ble::Link::Lan)), again);
    assert(aborted_reply(again, 0));
  }
  // A LOG_TAIL queued before the ABORT sends no LOG frame at all.
  reset_abort();
  Peer queued;
  const auto tail = stamped(request(ble::kOpLogTail, ble::Link::Ble, length));
  stamped(request(ble::kOpAbort, ble::Link::Ble));
  handle(tail, queued);
  assert(queued.successful.empty() && queued.errors.size() == 1 &&
         queued.errors[0].code == ble::kErrAborted);
  // An ABORT the full queue refused is stamped but never committed, so it
  // stops nothing and leaves no stopped op for the next marker.
  reset_abort();
  Peer refused;
  const auto kept = stamped(request(ble::kOpLogTail, ble::Link::Ble, length));
  auto lost = request(ble::kOpAbort, ble::Link::Ble);
  aqsync::abort::stamp(lost);
  handle(kept, refused);
  assert(refused.errors.empty() &&
         refused.successful.back().bytes[0] == ble::kFrameLogEnd);
  Peer next;
  handle(stamped(request(ble::kOpAbort, ble::Link::Ble)), next);
  assert(aborted_reply(next, 0));
  // An ABORT on the other link neither stops nor is named by this LOG_TAIL.
  reset_abort();
  Peer other;
  other.payload = 2000;
  other.abort_attempt = 1;
  other.abort_link = ble::Link::Lan;
  handle(stamped(request(ble::kOpLogTail, ble::Link::Ble, length)), other);
  assert(other.errors.empty() &&
         other.successful.back().bytes[0] == ble::kFrameLogEnd);
  Peer ble_marker;
  handle(stamped(request(ble::kOpAbort, ble::Link::Ble)), ble_marker);
  assert(aborted_reply(ble_marker, 0));

  // Stale marker: LOG_TAIL stopped under generation g, the marker belongs to
  // the replacement connection g+1 and must answer ABORTED 0.
  reset_abort();
  Peer stale;
  auto old_tail = request(ble::kOpLogTail, ble::Link::Lan, length);
  old_tail.link_generation = 7;
  old_tail = stamped(old_tail);
  auto old_abort = request(ble::kOpAbort, ble::Link::Lan);
  old_abort.link_generation = 7;
  stamped(old_abort);
  handle(old_tail, stale);
  assert(stale.errors.size() == 1 && stale.errors[0].code == ble::kErrAborted);
  auto new_marker = request(ble::kOpAbort, ble::Link::Lan);
  new_marker.link_generation = 8;
  Peer fresh;
  handle(stamped(new_marker), fresh);
  assert(aborted_reply(fresh, 0));

  // LIST queued before an ABORT, through the real ArchiveSession: ERROR
  // aborted only, then the marker answers ABORTED LIST, then ABORTED 0.
  reset_abort();
  Peer lan;
  aqsync::ArchiveHooks hooks{};
  hooks.list = [](void *, bool, unsigned, aqsync::FileEmitter, void *,
                  unsigned &) {
    assert(false && "an aborted LIST must not walk the archive");
    return aqsync::ListStatus::Failed;
  };
  aqsync::ArchiveTransport transport{};
  transport.context = &lan;
  transport.generation = [](void *, ble::Link, std::uint8_t) {
    return std::uint32_t(3);
  };
  transport.payload_max = [](void *, ble::Link, std::uint8_t) {
    return std::uint16_t(1024);
  };
  transport.respond = [](void *ctx, ble::Link link, std::uint8_t, std::uint32_t,
                         const std::uint8_t *bytes, std::size_t size) {
    return Peer::respond(ctx, link, bytes, size);
  };
  transport.error = [](void *ctx, ble::Link link, std::uint8_t, std::uint32_t,
                       ble::Op op, ble::Error code, const char *detail) {
    return Peer::error(ctx, link, op, code, detail);
  };
  aqsync::ArchiveSession session(hooks, transport);
  auto list = request(ble::kOpList, ble::Link::Lan);
  list.peer = 1;
  list.link_generation = 3;
  list = stamped(list);
  auto abort = request(ble::kOpAbort, ble::Link::Lan);
  abort.peer = 1;
  abort.link_generation = 3;
  abort = stamped(abort);
  assert(session.handle(list, true));
  assert(lan.successful.empty() && lan.errors.size() == 1 &&
         lan.errors[0].op == ble::kOpList &&
         lan.errors[0].code == ble::kErrAborted);
  assert(!session.handle(abort, true)); // ABORT is not an archive op
  Peer marker;
  handle(abort, marker);
  assert(aborted_reply(marker, ble::kOpList));
  Peer idle;
  handle(stamped(abort), idle);
  assert(aborted_reply(idle, 0));

  // ABORT while a LIST job runs. The marker waits for the job, the next step
  // reads nothing more and ends with ERROR aborted, then the marker answers
  // ABORTED LIST.
  reset_abort();
  static unsigned walk_reads;
  walk_reads = 0;
  lan = Peer{};
  hooks.list = [](void *, bool, unsigned budget, aqsync::FileEmitter emit,
                  void *out, unsigned &) {
    // An endless directory, so only ABORT can end this walk.
    for (unsigned read = 0; read < budget; ++read) {
      ++walk_reads;
      if (!emit(out, "a.parquet", 1))
        return aqsync::ListStatus::Stopped;
    }
    return aqsync::ListStatus::More;
  };
  aqsync::ArchiveSession walking(hooks, transport);
  auto long_list = request(ble::kOpList, ble::Link::Lan);
  long_list.peer = 1;
  long_list.link_generation = 3;
  long_list = stamped(long_list);
  assert(walking.handle(long_list, true) && walking.listing());
  walking.step(true);
  walking.step(true);
  assert(walk_reads == 2 * aqsync::ArchiveSession::kListStepEntries);
  assert(lan.errors.empty() &&
         lan.successful.size() == 2 * aqsync::ArchiveSession::kListStepEntries);
  auto mid_abort = request(ble::kOpAbort, ble::Link::Lan);
  mid_abort.peer = 1;
  mid_abort.link_generation = 3;
  mid_abort = stamped(mid_abort);
  assert(walking.must_wait_for_list(mid_abort));
  const unsigned reads = walk_reads;
  walking.step(true);
  assert(!walking.listing() && walk_reads == reads);
  assert(lan.errors.size() == 1 && lan.errors[0].op == ble::kOpList &&
         lan.errors[0].code == ble::kErrAborted);
  assert(!walking.must_wait_for_list(mid_abort));
  Peer stopped;
  handle(mid_abort, stopped);
  assert(aborted_reply(stopped, ble::kOpList));
  reset_abort();
}

void dispatch() {
  Peer peer;
  const auto callbacks = peer.callbacks();
  assert(!aqsync::handle_common_control(request(ble::kOpSetTime), callbacks));
  assert(!aqsync::handle_common_control(request(ble::kOpOpen), callbacks));
  ble::ControlRequest empty;
  assert(!aqsync::handle_common_control(empty, callbacks));
  assert(!aqsync::handle_common_control(request(ble::kOpGetConfig), {}));
  handle(request(ble::kOpWifiScan, ble::Link::Lan), peer);
  assert(peer.errors.size() == 1 && peer.errors[0].code == ble::kErrBusy);
  assert(peer.errors[0].link == ble::Link::Lan);
}
} // namespace

// These are transport observations/actions, not radio or protocol mirrors.
namespace ble {
LinkState link() {
  LinkState state;
  state.bonds = 3;
  return state;
}
void clear_bonds() { ++bonds_cleared; }
} // namespace ble
namespace lan {
Status status() {
  Status value;
  value.state = "connected";
  std::strcpy(value.ip, "192.0.2.1");
  std::strcpy(value.host, "aq-fixture");
  value.authenticated = true;
  value.clients = 3;
  return value;
}
void drop_session(ble::DisconnectReason reason) {
  assert(reason == ble::DisconnectReason::TokenRotated);
  ++sessions_dropped;
}
void apply_time_settings() { ++time_reapplied; }
void apply_settings() { ++wifi_reapplied; }
} // namespace lan

int main() {
  configuration();
  paged_configuration();
  config_chunks();
  token_guard();
  logs();
  diagnostics();
  aborts();
  dispatch();
}

namespace aq::network_time {
Status status() { return Status{}; }
} // namespace aq::network_time
