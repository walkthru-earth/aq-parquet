#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace fake_platform {
struct Nvs {
  std::map<std::string, std::uint32_t> numbers;
  std::map<std::string, std::string> strings;
  std::map<std::string, std::vector<std::uint8_t>> blobs;
  bool fail_open = false;
  std::string fail_write_key;
  unsigned writes = 0;
};
inline Nvs nvs;
} // namespace fake_platform

// Return values follow the pinned Arduino-ESP32 3.3.11 Preferences API:
// putString excludes its NUL; getString includes it; each put is a commit.
class Preferences {
public:
  bool begin(const char *name, bool) {
    opened_ = std::strcmp(name, "aqcfg") == 0 && !fake_platform::nvs.fail_open;
    return opened_;
  }
  void end() { opened_ = false; }
  bool isKey(const char *key) const {
    const auto &nvs = fake_platform::nvs;
    return opened_ && (nvs.numbers.count(key) || nvs.strings.count(key) ||
                       nvs.blobs.count(key));
  }
  std::size_t putUChar(const char *key, std::uint8_t value) {
    return put_number(key, value, 1);
  }
  std::size_t putUInt(const char *key, std::uint32_t value) {
    return put_number(key, value, 4);
  }
  std::uint8_t getUChar(const char *key, std::uint8_t fallback) const {
    return static_cast<std::uint8_t>(get_number(key, fallback));
  }
  std::uint32_t getUInt(const char *key, std::uint32_t fallback) const {
    return get_number(key, fallback);
  }
  std::size_t putString(const char *key, const char *value) {
    if (!allow_write(key))
      return 0;
    fake_platform::nvs.strings[key] = value;
    return std::strlen(value);
  }
  std::size_t getString(const char *key, char *out,
                        std::size_t capacity) const {
    const auto &strings = fake_platform::nvs.strings;
    const auto found = strings.find(key);
    if (!opened_ || found == strings.end() ||
        found->second.size() + 1 > capacity)
      return 0;
    std::memcpy(out, found->second.c_str(), found->second.size() + 1);
    return found->second.size() + 1;
  }
  std::size_t putBytes(const char *key, const void *data, std::size_t size) {
    if (!allow_write(key))
      return 0;
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    fake_platform::nvs.blobs[key] =
        std::vector<std::uint8_t>(bytes, bytes + size);
    return size;
  }
  std::size_t getBytes(const char *key, void *out, std::size_t capacity) const {
    const auto &blobs = fake_platform::nvs.blobs;
    const auto found = blobs.find(key);
    if (!opened_ || found == blobs.end() || found->second.size() > capacity)
      return 0;
    std::memcpy(out, found->second.data(), found->second.size());
    return found->second.size();
  }

private:
  bool opened_ = false;
  bool allow_write(const char *key) const {
    ++fake_platform::nvs.writes;
    return opened_ && fake_platform::nvs.fail_write_key != key;
  }
  std::size_t put_number(const char *key, std::uint32_t value,
                         std::size_t size) {
    if (!allow_write(key))
      return 0;
    fake_platform::nvs.numbers[key] = value;
    return size;
  }
  std::uint32_t get_number(const char *key, std::uint32_t fallback) const {
    const auto &numbers = fake_platform::nvs.numbers;
    const auto found = numbers.find(key);
    return opened_ && found != numbers.end() ? found->second : fallback;
  }
};
