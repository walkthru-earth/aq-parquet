#include "../firmware/common/sensors/src/aq_max17048.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>

namespace {
struct FakeBus {
  std::array<std::uint16_t, 32> registers{};
  std::vector<std::uint8_t> reads;
  std::vector<std::pair<std::uint8_t, std::uint16_t>> writes;
  int fail_reg = -1;
  std::uint32_t fail_mask = 0; // one bit per register
  bool fail_write = false;
  bool ignore_write = false; // the device ACKs but keeps the old value
  int fail_read_after_write = -1;

  aq::Max17048Io callbacks() { return {this, read}; }
  aq::Max17048Io read_write() { return {this, read, write}; }
  static bool read(void *context, std::uint8_t address, std::uint8_t reg,
                   std::uint8_t *data, std::size_t size) {
    auto &bus = *static_cast<FakeBus *>(context);
    assert(address == aq::Max17048::kAddress);
    assert(size == 2 && reg < bus.registers.size());
    bus.reads.push_back(reg);
    if (reg == bus.fail_reg || (bus.fail_mask & (1u << reg)) != 0)
      return false;
    if (!bus.writes.empty() && reg == bus.fail_read_after_write)
      return false;
    data[0] = static_cast<std::uint8_t>(bus.registers[reg] >> 8);
    data[1] = static_cast<std::uint8_t>(bus.registers[reg] & 0xff);
    return true;
  }
  static bool write(void *context, std::uint8_t address, std::uint8_t reg,
                    const std::uint8_t *data, std::size_t size) {
    auto &bus = *static_cast<FakeBus *>(context);
    assert(address == aq::Max17048::kAddress);
    assert(size == 2 && reg < bus.registers.size());
    const auto value = static_cast<std::uint16_t>((data[0] << 8) | data[1]);
    bus.writes.emplace_back(reg, value);
    if (bus.fail_write)
      return false;
    if (!bus.ignore_write)
      bus.registers[reg] = value;
    return true;
  }
};

// Decoding is usable at compile time.
static_assert(aq::Max17048::decode(0xd200, 0x6400).millivolts == 4200);
static_assert(aq::Max17048::decode(0xd200, 0x6400).percent == 100);
static_assert(aq::Max17048::decode(0xd200, 0x6400).valid());
static_assert(!aq::Max17048::decode(0, 0x5000).valid());
static_assert(!aq::Max17048::decode(0, 0x5000).vcell_valid());
static_assert(aq::Max17048::decode(0, 0x5000).soc_valid());
static_assert(aq::Max17048::decode(0xd200, 0xffff).vcell_valid());
static_assert(!aq::Max17048::decode(0xd200, 0xffff).soc_valid());

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
  const auto both = Max17048::decode(0xd200, 0x6400);
  assert(both.vcell_ok && both.soc_ok && both.vcell_plausible &&
         both.soc_plausible);
  // Plausibility is per field. A bad SOC keeps a good VCELL and the reverse.
  const auto bad_soc = Max17048::decode(0xa500, 0xffff); // 3300 mV
  assert(bad_soc.vcell_valid() && bad_soc.millivolts == 3300 &&
         !bad_soc.soc_valid() && bad_soc.soc_ok && !bad_soc.soc_plausible &&
         !bad_soc.valid());
  const auto bad_vcell = Max17048::decode(57613, 0x4b80);
  assert(!bad_vcell.vcell_valid() && bad_vcell.vcell_ok &&
         bad_vcell.soc_valid() && bad_vcell.percent == 75 &&
         !bad_vcell.valid());
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

  // Each basic register fails on its own. SOC is still read after a failed
  // VCELL, and a failed SOC keeps the decoded VCELL.
  bus.reads.clear();
  bus.fail_reg = aq::Max17048::kRegVcell;
  const auto no_vcell = gauge.read();
  assert(!no_vcell.vcell_ok && !no_vcell.vcell_valid() && !no_vcell.valid());
  assert(no_vcell.raw_vcell == 0 && no_vcell.millivolts == 0);
  assert(no_vcell.soc_valid() && no_vcell.raw_soc == 0x3280 &&
         no_vcell.percent == 50);
  assert((bus.reads == std::vector<std::uint8_t>{0x02, 0x04}));
  bus.fail_reg = aq::Max17048::kRegSoc;
  const auto no_soc = gauge.read();
  assert(no_soc.vcell_valid() && no_soc.raw_vcell == 0xd200 &&
         no_soc.millivolts == 4200);
  assert(!no_soc.soc_ok && !no_soc.soc_valid() && !no_soc.valid() &&
         no_soc.raw_soc == 0 && no_soc.percent == 0);
  bus.fail_reg = -1;
  bus.fail_mask =
      (1u << aq::Max17048::kRegVcell) | (1u << aq::Max17048::kRegSoc);
  const auto neither = gauge.read();
  assert(!neither.vcell_ok && !neither.soc_ok && !neither.valid());
  bus.fail_mask = 0;
  // SOC 0xFFFF reads fine but is implausible. VCELL stays valid.
  bus.registers[aq::Max17048::kRegSoc] = 0xffff;
  const auto wild_soc = gauge.read();
  assert(wild_soc.vcell_valid() && wild_soc.millivolts == 4200 &&
         wild_soc.soc_ok && !wild_soc.soc_valid() &&
         wild_soc.raw_soc == 0xffff);
  bus.registers[aq::Max17048::kRegSoc] = 0x3280;
  // A VCELL word outside the range keeps a good SOC.
  bus.registers[aq::Max17048::kRegVcell] = 0xffff;
  const auto wild_vcell = gauge.read();
  assert(wild_vcell.vcell_ok && !wild_vcell.vcell_valid() &&
         wild_vcell.soc_valid() && wild_vcell.percent == 50);
  bus.registers[aq::Max17048::kRegVcell] = 0xd200;
  bus.fail_reg = aq::Max17048::kRegVersion;
  version = 0x7777;
  assert(!gauge.read_version(version) && version == 0x7777);

  // Unset IO never dereferences a null callback.
  const aq::Max17048 unwired{aq::Max17048Io{}};
  const auto none = unwired.read();
  assert(!none.vcell_ok && !none.soc_ok && !none.valid());
  assert(!unwired.read_version(version));
}
void extended_reads() {
  using aq::Max17048;
  using Ext = aq::Max17048Extended;
  FakeBus bus;
  bus.registers[Max17048::kRegMode] = 0x1000;
  bus.registers[Max17048::kRegVersion] = 0x0012;
  bus.registers[Max17048::kRegHibrt] = 0x8030;
  bus.registers[Max17048::kRegConfig] = 0x971c;
  bus.registers[Max17048::kRegValrt] = 0x00ff;
  bus.registers[Max17048::kRegCrate] = 0x0005;
  bus.registers[Max17048::kRegStatus] = 0x0102;
  bus.registers[Max17048::kRegVcell] = 0xd200;
  bus.registers[Max17048::kRegSoc] = 0x3280;
  const Max17048 gauge{bus.callbacks()};
  auto ext = gauge.read_extended();
  assert((bus.reads ==
          std::vector<std::uint8_t>{0x06, 0x08, 0x0a, 0x0c, 0x14, 0x16, 0x1a}));
  assert(ext.mode.ok && ext.hibernating() && ext.version.raw == 0x0012 &&
         ext.hibrt.raw == 0x8030 && ext.config.raw == 0x971c &&
         ext.valrt.raw == 0x00ff && ext.status.raw == 0x0102);
  assert(ext.crate_milli_percent_per_hour() == 1040);
  assert(ext.status_bit(Ext::kStatusRi) && !ext.status_bit(Ext::kStatusVh));
  assert(ext.status_flags() == 0x01);

  // CRATE is signed, 0.208 %/h per LSB.
  bus.registers[Max17048::kRegCrate] = 0xfffb;
  assert(gauge.read_extended().crate_milli_percent_per_hour() == -1040);
  bus.registers[Max17048::kRegCrate] = 0x8000;
  assert(gauge.read_extended().crate_milli_percent_per_hour() == -6815744);
  bus.registers[Max17048::kRegCrate] = 0x7fff;
  assert(gauge.read_extended().crate_milli_percent_per_hour() == 6815536);
  bus.registers[Max17048::kRegCrate] = 0;
  assert(gauge.read_extended().crate_milli_percent_per_hour() == 0);

  // Every STATUS bit decodes on its own.
  const std::uint16_t bits[] = {Ext::kStatusRi,  Ext::kStatusVh, Ext::kStatusVl,
                                Ext::kStatusVr,  Ext::kStatusHd, Ext::kStatusSc,
                                Ext::kStatusEnVr};
  for (const auto bit : bits) {
    bus.registers[Max17048::kRegStatus] = bit;
    const auto one = gauge.read_extended();
    for (const auto other : bits)
      assert(one.status_bit(other) == (other == bit));
    assert(one.status_flags() == (bit >> 8));
  }
  bus.registers[Max17048::kRegMode] = 0x0000;
  assert(!gauge.read_extended().hibernating());

  // Raw SOC stays uncapped.
  assert(Max17048::raw_soc_percent(0x6bc9) > 107.7 &&
         Max17048::raw_soc_percent(0x6bc9) < 107.8);
  assert(Max17048::raw_soc_percent(0x0080) == 0.5);

  // Each register fails on its own and the rest are still read.
  const std::uint8_t regs[] = {0x06, 0x08, 0x0a, 0x0c, 0x14, 0x16, 0x1a};
  bus.registers[Max17048::kRegMode] = 0x1000;
  bus.registers[Max17048::kRegStatus] = 0x0100;
  for (const auto reg : regs) {
    bus.reads.clear();
    bus.fail_mask = 1u << reg;
    const auto partial = gauge.read_extended();
    assert(bus.reads.size() == 7);
    const aq::Max17048Word *words[] = {
        &partial.mode,  &partial.version, &partial.hibrt, &partial.config,
        &partial.valrt, &partial.crate,   &partial.status};
    for (std::size_t i = 0; i < 7; ++i)
      assert(words[i]->ok == (regs[i] != reg));
    if (reg == Max17048::kRegMode)
      assert(!partial.hibernating() && partial.mode.raw == 0);
    if (reg == Max17048::kRegStatus)
      assert(!partial.status_bit(Ext::kStatusRi));
    // A failed diagnostic register never nulls mV or percent.
    const auto basic = gauge.read();
    assert(basic.valid() && basic.millivolts == 4200 && basic.percent == 50);
  }
  bus.fail_mask = 0;

  // The basic read still touches only VCELL and SOC.
  bus.reads.clear();
  (void)gauge.read();
  assert((bus.reads == std::vector<std::uint8_t>{0x02, 0x04}));

  const aq::Max17048 unwired{aq::Max17048Io{}};
  const auto none = unwired.read_extended();
  assert(!none.mode.ok && !none.status.ok && !none.crate.ok);
}

void reset_indicator_clear() {
  using aq::Max17048;
  using Result = aq::Max17048ClearResult;
  {
    // RI is cleared and every other bit survives.
    FakeBus bus;
    bus.registers[Max17048::kRegStatus] = 0x7f00 | 0x00a5;
    const Max17048 gauge{bus.read_write()};
    assert(gauge.clear_reset_indicator() == Result::Cleared);
    assert(bus.writes.size() == 1 && bus.writes[0].first == 0x1a &&
           bus.writes[0].second == 0x7ea5);
    assert(bus.registers[Max17048::kRegStatus] == 0x7ea5);
    assert((bus.reads == std::vector<std::uint8_t>{0x1a, 0x1a}));
    // A second call writes nothing.
    bus.reads.clear();
    assert(gauge.clear_reset_indicator() == Result::AlreadyClear);
    assert(bus.writes.size() == 1);
    assert((bus.reads == std::vector<std::uint8_t>{0x1a}));
  }
  {
    // No write callback means no bus traffic at all.
    FakeBus bus;
    bus.registers[Max17048::kRegStatus] = 0x0100;
    const Max17048 gauge{bus.callbacks()};
    assert(gauge.clear_reset_indicator() == Result::WriteUnset);
    assert(bus.reads.empty() && bus.writes.empty());
  }
  {
    FakeBus bus;
    bus.registers[Max17048::kRegStatus] = 0x0100;
    bus.fail_reg = Max17048::kRegStatus;
    const Max17048 gauge{bus.read_write()};
    assert(gauge.clear_reset_indicator() == Result::ReadFailed);
    assert(bus.writes.empty());
  }
  {
    FakeBus bus;
    bus.registers[Max17048::kRegStatus] = 0x0300;
    bus.fail_write = true;
    const Max17048 gauge{bus.read_write()};
    assert(gauge.clear_reset_indicator() == Result::WriteFailed);
    assert(bus.registers[Max17048::kRegStatus] == 0x0300);
  }
  {
    FakeBus bus;
    bus.registers[Max17048::kRegStatus] = 0x0100;
    bus.ignore_write = true;
    const Max17048 gauge{bus.read_write()};
    assert(gauge.clear_reset_indicator() == Result::VerifyFailed);
  }
  {
    FakeBus bus;
    bus.registers[Max17048::kRegStatus] = 0x0100;
    bus.fail_read_after_write = Max17048::kRegStatus;
    const Max17048 gauge{bus.read_write()};
    assert(gauge.clear_reset_indicator() == Result::VerifyFailed);
    assert(bus.writes.size() == 1);
  }
}

static_assert(aq::clear_result_name(aq::Max17048ClearResult::Cleared)[0] ==
              'c');
static_assert(aq::clear_result_name(aq::Max17048ClearResult::VerifyFailed)[0] ==
              'v');

// The calibration entry point carries the default model only.
static_assert(aq::Max17048ModelCalibration{}.rcomp0 == 0x97);
static_assert(!aq::Max17048ModelCalibration{}.loaded);
} // namespace

int main() {
  decode_bounds();
  bus_reads();
  extended_reads();
  reset_indicator_clear();
  return 0;
}
