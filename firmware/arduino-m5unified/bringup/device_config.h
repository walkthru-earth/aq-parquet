#pragma once

// Owner-editable device settings, protocol v2
// (docs/shared/ble-sync-protocol.md, "Device configuration"). Stored in the NVS
// namespace `aqcfg`, separate from the `parquet` namespace that holds the
// station identity. Loaded once at boot on the main task; later reads and
// writes come from the storage worker (the only task that executes control
// requests), so the copy handed out by get() is guarded by a spinlock and
// everything else is single-writer.

#include <cstddef>
#include <cstdint>

namespace config {
enum class PairMode : std::uint8_t { Random = 0, Fixed = 1, None = 2 };
// Marker used only to migrate legacy installations that stored the old
// universal PIN. New devices receive a random per-device fixed PIN.
constexpr std::uint32_t kLegacyDefaultPin = 123456;
constexpr std::uint16_t kLanPort = 47390;
constexpr std::size_t kTokenBytes = 32;
constexpr std::size_t kSsidMax = 32;
constexpr std::size_t kPskMax = 63;

struct Settings {
  PairMode pair = PairMode::Random;
  std::uint32_t pin = 0;
  bool display = true; // detected at first boot, then frozen
  bool wifi_on = false;
  char ssid[kSsidMax + 1]{};
  char psk[kPskMax + 1]{};
  bool lan_on = true;
  std::uint8_t token[kTokenBytes]{};
};

// One-shot actions requested through SET_CONFIG; not stored.
struct Actions {
  bool clear_bonds = false;
  bool rotate_token = false;
  bool wifi_changed = false; // ssid/psk/on changed: LAN task must reapply
  bool ble_changed = false;  // pair/pin changed: takes effect at next boot
};

// Loads settings, seeding defaults on first boot from `display_detected`
// (screen -> random passkey, no screen -> random per-device fixed PIN) and
// generating the LAN token. Legacy universal PINs are rotated on load. Returns
// false when NVS was unusable, in which
// case defaults are used and nothing persists.
bool load(bool display_detected);

Settings get();
// Runtime view for JSON/UI: whether a `ble.*` change is waiting for a reboot.
bool reboot_required();

// Parses `key=value` lines (see the protocol table), validates all of them,
// then applies and saves. On failure nothing changes and `bad_key` names the
// culprit. `actions` reports what the caller must do next.
bool apply_lines(const char *text, std::size_t length, char *bad_key,
                 std::size_t bad_key_size, Actions &actions);

// Replaces the LAN token with fresh random bytes and saves.
bool rotate_token();

struct WifiView {
  const char *state; // off | connecting | connected | failed
  const char *ip;    // dotted quad or ""
  int rssi;
  const char *mac;
  unsigned clients;
  const char *host; // mDNS host label
  unsigned bonds;
};
// Builds the CONFIG JSON (<= 400 bytes). Never includes the PSK or token.
std::size_t build_json(char *out, std::size_t size, const WifiView &wifi);

const char *pair_name(PairMode mode);
} // namespace config
