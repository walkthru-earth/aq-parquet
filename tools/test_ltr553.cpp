#include "../firmware/common/src/aq_ltr553.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace {
struct FakeIo {
  std::array<std::uint8_t, 256> registers{};
  std::vector<std::pair<std::uint8_t, std::uint8_t>> writes;
  std::vector<std::uint8_t> reads;
  std::uint32_t clock = 0;
  int fail_read = -1;
  int fail_write_index = -1;

  FakeIo() {
    registers[0x86] = 0x92;
    registers[0x87] = 0x05;
  }
  aq::Ltr553Io callbacks() { return {this, read, write, delay_ms, millis}; }
  static bool read(void *context, std::uint8_t address, std::uint8_t reg,
                   std::uint8_t *data, std::size_t size) {
    auto &io = *static_cast<FakeIo *>(context);
    assert(address == 0x23);
    assert(static_cast<std::size_t>(reg) + size <= io.registers.size());
    io.reads.push_back(reg);
    if (reg == io.fail_read)
      return false;
    std::copy_n(io.registers.data() + reg, size, data);
    return true;
  }
  static bool write(void *context, std::uint8_t address, std::uint8_t reg,
                    std::uint8_t value) {
    auto &io = *static_cast<FakeIo *>(context);
    assert(address == 0x23);
    io.writes.emplace_back(reg, value);
    return static_cast<int>(io.writes.size() - 1) != io.fail_write_index;
  }
  static void delay_ms(void *context, std::uint32_t milliseconds) {
    assert(milliseconds == 100);
    static_cast<FakeIo *>(context)->clock += milliseconds;
  }
  static std::uint32_t millis(void *context) {
    return static_cast<FakeIo *>(context)->clock;
  }
};

void test_setup() {
  const std::vector<std::pair<std::uint8_t, std::uint8_t>> expected = {
      {0x80, 0x00}, {0x81, 0x00}, {0x8f, 0x00}, {0x82, 0x2a}, {0x83, 0x01},
      {0x84, 0x05}, {0x85, 0x04}, {0x80, 0x01}, {0x81, 0x23}};
  FakeIo io;
  aq::Ltr553 sensor(io.callbacks());
  assert(!sensor.available());
  assert(!sensor.read().io_error && io.reads.empty());
  assert(sensor.begin() && sensor.available());
  assert(io.clock == 100 && io.writes == expected);
  io.clock += 1099;
  assert(!sensor.read().als_valid && io.reads.size() == 1);
  ++io.clock;
  sensor.read();
  assert(io.reads.back() == 0x8c);

  for (std::size_t index = 0; index < expected.size(); ++index) {
    FakeIo failing;
    failing.fail_write_index = static_cast<int>(index);
    aq::Ltr553 broken(failing.callbacks());
    assert(!broken.begin() && !broken.available());
    assert(failing.writes.size() == index + 3);
    assert(failing.writes[index] == expected[index]);
    assert(failing.writes[index + 1] == expected[0]);
    assert(failing.writes[index + 2] == expected[1]);
  }
  for (int reg : {0x86, 0x87}) {
    FakeIo wrong_id;
    wrong_id.registers[reg] ^= 1;
    aq::Ltr553 broken(wrong_id.callbacks());
    assert(!broken.begin() && wrong_id.writes.empty());
  }
  FakeIo failed_id;
  failed_id.fail_read = 0x86;
  aq::Ltr553 broken(failed_id.callbacks());
  assert(!broken.begin() && failed_id.writes.empty());
  aq::Ltr553 disconnected({nullptr, nullptr, nullptr, nullptr, nullptr});
  assert(!disconnected.begin() && !disconnected.read().als_valid);
}

void test_readings() {
  FakeIo io;
  io.clock = std::numeric_limits<std::uint32_t>::max() - 150;
  aq::Ltr553 sensor(io.callbacks());
  assert(sensor.begin());
  io.clock += 1100; // A wraparound is a normal warmup deadline.
  io.registers[0x88] = 0x34;
  io.registers[0x89] = 0x12;
  io.registers[0x8a] = 0x78;
  io.registers[0x8b] = 0x56;
  io.registers[0x8d] = 0xff;
  io.registers[0x8e] = 0x07;
  io.registers[0x8c] = 0x05;
  auto reading = sensor.read();
  assert(reading.als_valid && reading.proximity_valid && !reading.io_error);
  assert(reading.als_ch1 == 0x1234 && reading.als_ch0 == 0x5678);
  assert(reading.proximity == 2047);
  assert(reading.als_fresh && reading.proximity_fresh);
  for (std::uint8_t bad_gain_or_validity : {0x10, 0x20, 0x40, 0x80}) {
    io.registers[0x8c] = bad_gain_or_validity | 0x05;
    reading = sensor.read();
    assert(!reading.als_valid && reading.proximity_valid);
  }
  io.registers[0x8c] = 0x05;
  io.registers[0x8e] = 0xff;
  reading = sensor.read();
  assert(reading.proximity_saturated && !reading.proximity_valid);
  assert(reading.proximity == 2047 && reading.als_valid);
  io.registers[0x8e] = 0x07;
  io.registers[0x8c] = 0;
  io.reads.clear();
  reading = sensor.read();
  assert(io.reads == std::vector<std::uint8_t>{0x8c});
  assert(!reading.als_fresh && !reading.proximity_fresh);
  assert(!reading.als_valid && !reading.proximity_valid);
  assert(reading.als_ch0 == 0 && reading.proximity == 0); // No stale cache.
  io.registers[0x8c] = 0x05;
  for (int reg : {0x8c, 0x88, 0x8d}) {
    io.fail_read = reg;
    reading = sensor.read();
    assert(reading.io_error);
    assert(reading.als_valid == (reg == 0x8d));
    assert(reading.proximity_valid == (reg == 0x88));
  }
  io.fail_read = -1;
  assert(sensor.read().als_valid); // Transient I/O failures can recover.
}
} // namespace

int main() {
  test_setup();
  test_readings();
}
