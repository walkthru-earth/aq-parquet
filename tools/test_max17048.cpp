#include "../firmware/common/sensors/src/aq_max17048.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

namespace {
struct FakeBus {
  std::array<std::uint16_t, 16> registers{};
  std::vector<std::uint8_t> reads;
  int fail_reg = -1;

  aq::Max17048Io callbacks() { return {this, read}; }
  static bool read(void *context, std::uint8_t address, std::uint8_t reg,
                   std::uint8_t *data, std::size_t size) {
    auto &bus = *static_cast<FakeBus *>(context);
    assert(address == aq::Max17048::kAddress);
    assert(size == 2 && reg < bus.registers.size());
    bus.reads.push_back(reg);
    if (reg == bus.fail_reg)
      return false;
    data[0] = static_cast<std::uint8_t>(bus.registers[reg] >> 8);
    data[1] = static_cast<std::uint8_t>(bus.registers[reg] & 0xff);
    return true;
  }
};

// Decoding is usable at compile time.
static_assert(aq::Max17048::decode(0xd200, 0x6400).millivolts == 4200);
static_assert(aq::Max17048::decode(0xd200, 0x6400).percent == 100);
static_assert(aq::Max17048::decode(0xd200, 0x6400).valid());
static_assert(!aq::Max17048::decode(0, 0x5000).valid());

void decode_bounds() {
  using aq::Max17048;
  const auto nominal = Max17048::decode(0xc800, 0x4b80);
  assert(nominal.valid() && nominal.millivolts == 4000 &&
         nominal.percent == 75 && nominal.raw_vcell == 0xc800 &&
         nominal.raw_soc == 0x4b80);
  // 2500 and 4500 mV are both accepted, just outside is not.
  assert(Max17048::decode(32000, 0).valid());
  assert(!Max17048::decode(31999, 0).valid());
  assert(Max17048::decode(57600, 0).valid());
  assert(!Max17048::decode(57613, 0).valid());
  // Above 100% is capped, above 120% raw SOC is implausible.
  const auto over_full = Max17048::decode(0xd5c0, 0x6bc9);
  assert(over_full.valid() && over_full.percent == 100 &&
         over_full.millivolts == 4275);
  assert(Max17048::decode(0xd200, 120 * 256).valid());
  assert(!Max17048::decode(0xd200, 120 * 256 + 1).valid());
  assert(!Max17048::decode(0xd200, 0xffff).valid());
  assert(Max17048::decode(0xd200, 0x00ff).percent == 0); // truncated
  assert(!Max17048::decode(0xd200, 0x6400).io_error);
}

void bus_reads() {
  FakeBus bus;
  bus.registers[aq::Max17048::kRegVcell] = 0xd200;
  bus.registers[aq::Max17048::kRegSoc] = 0x3280;
  bus.registers[aq::Max17048::kRegVersion] = 0x0012;
  const aq::Max17048 gauge{bus.callbacks()};
  const auto reading = gauge.read();
  assert(reading.valid() && reading.millivolts == 4200 &&
         reading.percent == 50);
  assert((bus.reads == std::vector<std::uint8_t>{0x02, 0x04}));
  std::uint16_t version = 0;
  assert(gauge.read_version(version) && version == 0x0012);

  // A failed VCELL read skips SOC. A failed SOC read keeps the VCELL word.
  bus.reads.clear();
  bus.fail_reg = aq::Max17048::kRegVcell;
  const auto no_vcell = gauge.read();
  assert(no_vcell.io_error && !no_vcell.valid());
  assert((bus.reads == std::vector<std::uint8_t>{0x02}));
  bus.fail_reg = aq::Max17048::kRegSoc;
  const auto no_soc = gauge.read();
  assert(no_soc.io_error && !no_soc.valid() && no_soc.raw_vcell == 0xd200 &&
         no_soc.millivolts == 0 && no_soc.percent == 0);
  bus.fail_reg = aq::Max17048::kRegVersion;
  version = 0x7777;
  assert(!gauge.read_version(version) && version == 0x7777);

  // Unset IO never dereferences a null callback.
  const aq::Max17048 unwired{aq::Max17048Io{}};
  assert(unwired.read().io_error);
  assert(!unwired.read_version(version));
}
} // namespace

int main() {
  decode_bounds();
  bus_reads();
  return 0;
}
