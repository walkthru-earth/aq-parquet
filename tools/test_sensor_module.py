#!/usr/bin/env python3
"""Generated module headers and ossensor::resolve on the host (#17 step 4).

For each board, a translation unit includes the board telemetry_contract.h,
os_sensor_module.h and the checked-in header of every module the board.json
lists. It resolves each module's kColumns against std::span{kFields} at
compile time, static_asserts the indices equal the positions the plugin gate
expands, and checks at run time that each index names its column and that
each Column enumerator is its kColumns position.

A second unit compiles the example BME280 header, which has an InitError,
into a Module table with a fake init and collect.

A third unit resolves the real max17048 header against a table with only its
core columns, selecting only the core segment. Core resolves and v5 maps to
kNoSlot.

A fourth unit runs the write helpers of section 10 on the same core only
table. The runtime integer, counter and number skip kNoSlot and any slot past
the row, ossensor::Writer writes the selected columns and compiles the v5
ones to nothing, including an unselected Int64 counter, and init_module
passes a Module::init result through.

Negative compiles prove that resolve() stops the build. A CoreS3 only module
resolved against the Waveshare table, an unknown column against the CoreS3
table and a selected v5 segment the table lacks must all fail, naming the
missing column guard. A Writer call of another type than the board field
must fail naming the type guard, a Writer call with a Column of another
module must fail naming the module guard, and a dropped init_module result
must fail as nodiscard.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import tempfile

import plugin_contract as gate
from host_cxx import CXX_STD

ROOT = gate.ROOT
COMMON = ROOT / "firmware/common"
INCLUDES = ("src", "sensors/src", "logger/src", "connectivity/src", "runtime/src", "location/src", "power/src")
# Each board app's telemetry_contract.h holds its column table here.
FIELDS = "telemetry::contract::kFields"
CORES3 = "m5stack-cores3"
WAVESHARE = "waveshare-esp32-s3-sim7670g"
MISSING = "module_column_missing_from_board_fields"
TYPE_DIFFERS = "module_column_type_differs_from_board_field"
FOREIGN = "the Column belongs to another module than this Writer"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def compiler(sanitize: bool, include: Path) -> list[str]:
    command = ["clang++", CXX_STD, "-Wall", "-Wextra", "-Werror", "-I", str(include)]
    for directory in INCLUDES:
        command += ["-I", str(COMMON / directory)]
    if sanitize:
        command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    return command


def board_unit(columns: list[gate.Column], modules: dict[str, gate.Module]) -> tuple[str, int]:
    position = {c.name: index for index, c in enumerate(columns)}
    used = list(dict.fromkeys(c.module for c in columns if c.module != "system"))
    lines = ['#include "telemetry_contract.h"', "#include <os_sensor_module.h>", "#include <cstdio>",
             "#include <span>", "#include <string_view>", "#include <utility>"]
    lines += [f'#include "{gate.header_path(modules[m]).as_posix()}"' for m in used]
    lines += ["", "namespace {", f"constexpr std::span kBoard{{{FIELDS}}};"]
    checks = []
    for module_id in used:
        segments = modules[module_id].manifest["segments"]
        names = [c["name"] for cols in segments.values() for c in cols]
        slots = ", ".join(str(position[n]) for n in names)
        ns = f"ossensor::{module_id}"
        lines += [f"constexpr auto k_{module_id} = ossensor::resolve({ns}::kColumns, kBoard);",
                  f"static_assert(k_{module_id} == ossensor::Slots<{len(names)}>{{{slots}}},",
                  f'              "{module_id} resolves to the positions the plugin gate expands");']
        for index, name in enumerate(names):
            lines.append(f"static_assert(std::to_underlying({ns}::Column::{name}) == {index});")
        # kColumnSegments follows the manifest, and selecting every segment
        # the board ships resolves the same slots.
        tags = ", ".join(f"{ns}::Segment::{s}" for s, cols in segments.items() for _ in cols)
        lines.append(f"static_assert({ns}::kColumnSegments == std::array{{{tags}}});")
        shipped = [s for s, cols in segments.items() if all(c["name"] in position for c in cols)]
        selected = ", ".join(f"{ns}::Segment::{s}" for s in shipped)
        lines += [f"static_assert(ossensor::resolve({ns}::kColumns, {ns}::kColumnSegments,",
                  f"                                std::array{{{selected}}}, kBoard) == k_{module_id});"]
        checks.append(f"  failures += check(k_{module_id}, {ns}::kColumns);")
    lines += [
        "template <std::size_t N>",
        "int check(const ossensor::Slots<N> &slots, const std::array<std::string_view, N> &names) {",
        "  int failures = 0;",
        "  for (std::size_t i = 0; i < N; ++i) {",
        "    if (std::string_view{kBoard[slots[i]].name} != names[i]) {",
        '      std::fprintf(stderr, "slot %zu does not name its column\\n", i);',
        "      ++failures;",
        "    }",
        "  }",
        "  return failures;",
        "}",
        "} // namespace",
        "",
        "int main() {",
        "  int failures = 0;",
        *checks,
        "  return failures == 0 ? 0 : 1;",
        "}",
        "",
    ]
    return "\n".join(lines), len(used)


EXAMPLE = """#include <os_sensor_module.h>
#include "bme280_module.h"
#include <cstdio>
#include <span>
#include <utility>

namespace {
struct Name {
  const char *name;
};
constexpr Name kTable[] = {{"sequence"}, {"bme280_status"}, {"air_temperature_c"},
                           {"relative_humidity_percent"}, {"air_pressure_hpa"}};
namespace bme = ossensor::bme280;
static_assert(bme::kColumns.size() == 3);
constexpr auto kSlots = ossensor::resolve(bme::kColumns, std::span{kTable});
static_assert(kSlots == ossensor::Slots<3>{1, 2, 4});
// A module column at index 0, so a search that skips the first row fails.
constexpr Name kFirst[] = {{"air_pressure_hpa"}, {"bme280_status"}, {"sequence"}, {"air_temperature_c"}};
static_assert(ossensor::resolve(bme::kColumns, std::span{kFirst}) == ossensor::Slots<3>{1, 3, 0});
static_assert(std::to_underlying(bme::InitError::not_found) == 0);
static_assert(std::to_underlying(bme::InitError::bad_chip_id) == 1);
static_assert(bme::kStatusColumn == bme::Column::bme280_status);
static_assert(bme::kStatusMissing == 0 && bme::kStatusReadError == 1 && bme::kStatusValid == 2);
static_assert(bme::kProcedure == "bme280");

struct Context {
  ossensor::Slots<3> slots;
  bool present;
};
Context present{.slots = kSlots, .present = true};
Context absent{.slots = kSlots, .present = false};

std::expected<void, ossensor::InitErrorCode> init(void *context) {
  if (!static_cast<Context *>(context)->present)
    return std::unexpected(ossensor::init_error(bme::InitError::bad_chip_id));
  return {};
}
void collect(oslogger::Row &row, std::int64_t, void *context) {
  const auto &slots = static_cast<Context *>(context)->slots;
  row.integer(slots[std::to_underlying(bme::Column::bme280_status)], bme::kStatusValid);
  row.number(slots[std::to_underlying(bme::Column::air_temperature_c)], 21.5F);
}
constexpr std::array kModules{
    ossensor::Module{.procedure = bme::kProcedure, .init = init, .collect = collect, .context = &present},
    ossensor::Module{.procedure = bme::kProcedure, .init = init, .collect = collect, .context = &absent},
};
} // namespace

int main() {
  oslogger::Row row{};
  for (const auto &module : kModules)
    if (module.init(module.context).has_value())
      module.collect(row, 0, module.context);
  const auto failed = kModules[1].init(kModules[1].context);
  const bool ok = kModules[0].init(kModules[0].context).has_value() && !failed.has_value() &&
                  std::to_underlying(failed.error()) == std::to_underlying(bme::InitError::bad_chip_id) &&
                  row.valid[1] == 1 && row.data[1] == bme::kStatusValid && row.valid[2] == 1 &&
                  row.valid[3] == 0 && row.valid[0] == 0;
  if (!ok)
    std::fprintf(stderr, "example module table wrote the wrong row\\n");
  return ok ? 0 : 1;
}
"""


# A table with only the max17048 core columns. {selected} picks segments.
CORE_ONLY = """#include <os_sensor_module.h>
#include "{header}"
#include <span>

namespace {{
struct Name {{
  const char *name;
}};
constexpr Name kTable[] = {{{{"sequence"}}, {{"gauge_status"}}, {{"battery_mv"}}, {{"battery_percent"}}}};
namespace g = ossensor::max17048;
[[maybe_unused]] constexpr auto kSlots =
    ossensor::resolve(g::kColumns, g::kColumnSegments, std::array{{{selected}}}, std::span{{kTable}});
static_assert(kSlots == ossensor::Slots<7>{{1, 2, 3, ossensor::kNoSlot, ossensor::kNoSlot, ossensor::kNoSlot,
                                            ossensor::kNoSlot}});
}} // namespace
"""


# The section 10 write helpers on a max17048 core only table with types.
# {write} is one extra statement for the negative compiles.
WRITER = """#include <os_sensor_module.h>
#include "{header}"
#include <cstdio>
#include <span>
#include <utility>

namespace {{
using telemetry::PhysicalType;
struct Field {{
  const char *name;
  PhysicalType type;
}};
constexpr Field kTable[] = {{{{"sequence", PhysicalType::Int64}},
                            {{"gauge_status", PhysicalType::Int32}},
                            {{"battery_mv", {mv_type}}},
                            {{"battery_percent", PhysicalType::Int32}}}};
namespace g = ossensor::max17048;
constexpr auto kSlots =
    ossensor::resolve(g::kColumns, g::kColumnSegments, std::array{{g::Segment::core}}, std::span{{kTable}});
// No max17048 column is Int64, so a local module Column carries one the
// board does not select.
enum class Local : std::uint8_t {{ sequence, skipped }};
constexpr ossensor::Slots<2> kLocalSlots{{{{0, ossensor::kNoSlot}}}};
std::expected<void, ossensor::InitErrorCode> refuse(void *) {{
  return std::unexpected(ossensor::InitErrorCode{{3}});
}}
bool empty(const oslogger::Row &row) {{
  for (std::size_t i = 0; i < row.data.size(); ++i)
    if (row.data[i] != 0 || row.valid[i] != 0)
      return false;
  return true;
}}
}} // namespace

int main() {{
  oslogger::Row row{{}};
  // kNoSlot and every slot past the row leave the row untouched.
  for (const unsigned slot : {{unsigned{{ossensor::kNoSlot}}, unsigned(row.data.size()), 200U}}) {{
    ossensor::integer(row, static_cast<ossensor::Slot>(slot), 7);
    ossensor::counter(row, static_cast<ossensor::Slot>(slot), 7);
    ossensor::number(row, static_cast<ossensor::Slot>(slot), 7.0F);
  }}
  bool ok = empty(row);
  ossensor::counter(row, kSlots[std::to_underlying(g::Column::gauge_status)] - 1, 9);
  ok = ok && row.valid[0] == 1 && row.data[0] == 9;
  row = {{}};
  const ossensor::Writer<g::Column, kTable, kSlots> out{{row}};
  out.integer<g::Column::gauge_status>(g::kStatusValid);
  out.integer<g::Column::battery_mv>(4100);
  // v5 is not selected, so these compile to no write.
  out.number<g::Column::battery_soc_raw_percent>(50.0F);
  out.integer<g::Column::gauge_status_flags>(1);
  out.integer<g::Column::gauge_hibernating>(1);
  {write}
  ok = ok && row.valid[1] == 1 && row.data[1] == g::kStatusValid && row.valid[2] == 1 && row.data[2] == 4100;
  row.valid[1] = row.valid[2] = 0;
  row.data[1] = row.data[2] = 0;
  ok = ok && empty(row);
  // A counter of a column the board does not select compiles to no write.
  const ossensor::Writer<Local, kTable, kLocalSlots> local{{row}};
  local.counter<Local::skipped>(5);
  ok = ok && empty(row);
  local.counter<Local::sequence>(11);
  ok = ok && row.valid[0] == 1 && row.data[0] == 11;
  row = {{}};
  const ossensor::Module none{{.procedure = g::kProcedure}};
  const ossensor::Module refused{{.procedure = g::kProcedure, .init = refuse}};
  ok = ok && ossensor::init_module(none).has_value();
  const auto failed = ossensor::init_module(refused);
  ok = ok && !failed.has_value() && std::to_underlying(failed.error()) == 3;
  if (!ok)
    std::fprintf(stderr, "the write helpers wrote the wrong row\\n");
  return ok ? 0 : 1;
}}
"""


def run(command: list[str], what: str) -> None:
    result = subprocess.run(command, capture_output=True, text=True)
    require(result.returncode == 0, f"{what} failed\n{result.stdout}{result.stderr}")


def rejects(command: list[str], what: str, needle: str = MISSING) -> None:
    result = subprocess.run(command, capture_output=True, text=True)
    require(result.returncode != 0, f"{what} compiled")
    require(needle in result.stderr, f"{what} failed for another reason\n{result.stderr}")
    print(f"PASS rejects {what}", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    # The checked-in headers must be current, or this would test stale bytes.
    gate.check(ROOT)
    checks = gate.validators(ROOT)
    common = gate.common_modules(ROOT, checks)
    boards = [gate.load_board(ROOT, path, checks, common) for path in sorted(ROOT.glob(gate.BOARD_GLOB))]
    require({CORES3, WAVESHARE} <= {b.manifest["board"] for b in boards}, "both shipped boards load")
    with tempfile.TemporaryDirectory(prefix="sensor-module-test-") as temporary:
        directory = Path(temporary)
        units = {}
        for board in boards:
            label = board.manifest["board"]
            modules = {**common, **board.modules}
            unit, count = board_unit(gate.expand(board, common), modules)
            source = directory / f"{label}.cpp"
            source.write_text(unit)
            executable = directory / label
            run([*compiler(args.sanitize, board.directory), str(source), "-o", str(executable)], f"{label} unit")
            run([str(executable)], f"{label} run")
            units[label] = board
            print(f"PASS {label} resolves {count} module headers against its kFields", flush=True)

        # The example module carries init_errors, which no board module has yet.
        example = gate.load_module(ROOT, ROOT / gate.EXAMPLES / "bme280.module.json", checks)
        (directory / "bme280_module.h").write_text(gate.module_header(ROOT, example))
        source = directory / "example.cpp"
        source.write_text(EXAMPLE)
        executable = directory / "example"
        run([*compiler(args.sanitize, directory), str(source), "-o", str(executable)], "example unit")
        run([str(executable)], "example run")
        print("PASS the example InitError, status constants and Module table compile and run", flush=True)

        # resolve() stops the build when a name is missing.
        cores3 = units[CORES3]
        waveshare = units[WAVESHARE]
        imu = gate.header_path(cores3.modules["m5_imu"]).as_posix()
        wrong = directory / "wrong-board.cpp"
        wrong.write_text(f'#include "telemetry_contract.h"\n#include <os_sensor_module.h>\n#include "{imu}"\n'
                         f"[[maybe_unused]] constexpr auto kSlots = ossensor::resolve(ossensor::m5_imu::kColumns, "
                         f"std::span{{{FIELDS}}});\n")
        rejects([*compiler(False, waveshare.directory), "-fsyntax-only", str(wrong)],
                "a CoreS3 module resolved against the Waveshare kFields")
        unknown = directory / "unknown-column.cpp"
        unknown.write_text('#include "telemetry_contract.h"\n#include <os_sensor_module.h>\n'
                           "constexpr std::array<std::string_view, 2> kNames{\"sequence\", \"no_such_column\"};\n"
                           "[[maybe_unused]] constexpr auto kSlots = ossensor::resolve(kNames, "
                           f"std::span{{{FIELDS}}});\n")
        rejects([*compiler(False, cores3.directory), "-fsyntax-only", str(unknown)],
                "an unknown column against the CoreS3 kFields")
        # The same unit with a column the board carries compiles, so the
        # rejection above comes from the missing name.
        unknown.write_text(unknown.read_text().replace("no_such_column", "monotonic_us"))
        run([*compiler(False, cores3.directory), "-fsyntax-only", str(unknown)], "a known column unit")
        print("PASS the same unit with a known column compiles", flush=True)

        # A board that selects only the max17048 core segment, as the weather
        # example does, resolves core and leaves v5 at kNoSlot.
        gauge = gate.header_path(common["max17048"]).as_posix()
        partial = directory / "core-only.cpp"
        partial.write_text(CORE_ONLY.format(header=gauge, selected="g::Segment::core"))
        run([*compiler(False, directory), "-fsyntax-only", str(partial)], "a core only max17048 unit")
        print("PASS a board selecting only max17048 core resolves core and leaves v5 at kNoSlot", flush=True)
        partial.write_text(CORE_ONLY.format(header=gauge, selected="g::Segment::core, g::Segment::v5"))
        rejects([*compiler(False, directory), "-fsyntax-only", str(partial)],
                "a selected max17048 v5 segment the board table lacks")

        # The write helpers of section 10 on the same core only table.
        writer = directory / "writer.cpp"
        int32 = "PhysicalType::Int32"
        writer.write_text(WRITER.format(header=gauge, mv_type=int32, write=""))
        run([*compiler(args.sanitize, directory), str(writer), "-o", str(directory / "writer")], "writer unit")
        run([str(directory / "writer")], "writer run")
        print("PASS the write helpers skip kNoSlot and past-the-row slots, Writer writes core only, "
              "init_module passes results", flush=True)
        writer.write_text(WRITER.format(header=gauge, mv_type="PhysicalType::Float", write=""))
        rejects([*compiler(False, directory), "-fsyntax-only", str(writer)],
                "an integer write to a Float board field", TYPE_DIFFERS)
        writer.write_text(WRITER.format(header=gauge, mv_type=int32,
                                        write="out.counter<g::Column::battery_percent>(1);"))
        rejects([*compiler(False, directory), "-fsyntax-only", str(writer)],
                "a counter write to an Int32 board field", TYPE_DIFFERS)
        writer.write_text(WRITER.format(header=gauge, mv_type=int32,
                                        write="out.integer<Local::sequence>(1);"))
        rejects([*compiler(False, directory), "-fsyntax-only", str(writer)],
                "a write of another module's Column", FOREIGN)
        dropped = "ossensor::init_module({.procedure = g::kProcedure, .init = refuse});"
        writer.write_text(WRITER.format(header=gauge, mv_type=int32, write=dropped))
        rejects([*compiler(False, directory), "-fsyntax-only", str(writer)],
                "a dropped init_module result", "nodiscard")


if __name__ == "__main__":
    main()
