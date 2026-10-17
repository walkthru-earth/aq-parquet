#pragma once

// BLE link authentication policy (issue #51). Pure decisions with no host
// stack or RTOS dependency, so ble_sync.cpp and the host tests share them.
//
// NimBLE's security state for the connection is the truth. The transport keeps
// an atomic copy so the response gates need no host lock per frame, and that
// copy used to change only on CONNECT and ENC_CHANGE. On some bonded
// reconnects ENC_CHANGE never reached the GAP callback, so the copy stayed
// false while every GATT access saw a secure link. The first LIST was then
// refused with ERROR 7 and that ERROR was dropped by the same stale gate.
//
// Now every host task place that reads the live state (CONNECT, ENC_CHANGE,
// SUBSCRIBE and each GATT access) latches the copy, and each gate falls back
// to the live state before it refuses. An insecure link is still refused,
// never with 7. An unencrypted write gets NimBLE's ATT error, which makes the
// phone pair. A Control write that passes ATT but not secure() gets ERROR 14.

#include "ble_sync.h"

#include <cstdint>
#include <span>

namespace ble {
// One live reading of NimBLE's security state on the host task. True when it
// is the first proof that the current link is secure, so the transport sets
// its flag, logs BLE BONDED once and starts the link sequence.
[[nodiscard]] constexpr bool should_latch(bool current_link, bool cached,
                                          bool secure_now) {
  return current_link && secure_now && !cached;
}

// The ERROR 14 refusal: ERROR type, any op, code kErrAuth. It names a fixed
// protocol code and constant detail, so it carries no secret.
[[nodiscard]] constexpr bool
is_auth_refusal(std::span<const std::uint8_t> frame) {
  return frame.size() >= 3 && frame[0] == kFrameError && frame[2] == kErrAuth;
}

// The response gates. A frame goes out when the cached flag says the link is
// authenticated, when it is the ERROR 14 refusal (the same way the LAN link
// answers a bad token before it closes), or else when NimBLE's live state says
// the link is secure. `live` runs only when nothing cheaper decided, so a
// healthy link pays no host lock per frame.
template <class Live>
[[nodiscard]] bool may_send(bool cached, std::span<const std::uint8_t> frame,
                            Live &&live) {
  return cached || is_auth_refusal(frame) || live();
}

// The ERROR that answers a Control write the transport does not hand to the
// worker. Authentication is decided first, so a link that is not secure gets
// 14 and code 7 only ever means the command queue was full.
struct Refusal {
  Error code;
  const char *detail; // constant, secret free
};
[[nodiscard]] constexpr Refusal control_refusal(bool secure_now) {
  return secure_now ? Refusal{.code = kErrBusy, .detail = "queue-full"}
                    : Refusal{.code = kErrAuth, .detail = "auth"};
}
} // namespace ble
