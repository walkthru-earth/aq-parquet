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

Negative compiles prove that resolve() stops the build. A CoreS3 only module
resolved against the Waveshare table, an unknown column against the CoreS3
table and a selected v5 segment the table lacks must all fail, naming the
missing column guard.
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


def run(command: list[str], what: str) -> None:
    result = subprocess.run(command, capture_output=True, text=True)
    require(result.returncode == 0, f"{what} failed\n{result.stdout}{result.stderr}")


def rejects(command: list[str], what: str) -> None:
    result = subprocess.run(command, capture_output=True, text=True)
    require(result.returncode != 0, f"{what} compiled")
    require(MISSING in result.stderr, f"{what} failed for another reason\n{result.stderr}")
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


if __name__ == "__main__":
    main()
