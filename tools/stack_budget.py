"""Static stack worst case per task and linker section sizes per image (issue #37).

Board builds compile every object with `-fstack-usage -fcallgraph-info=su`
(firmware/cmake/aq_stack_usage.cmake), so GCC leaves a `.ci` call graph with
each function's frame size next to every object. This tool joins those graphs,
walks them from each tracked task's entry and prints the deepest path. Functions
with no `.ci` (prebuilt newlib, libgcc and the Wi-Fi and Bluetooth blobs) take
their frame from the `entry a1, N` instruction and their calls from the
disassembly of the linked ELF. ROM functions have neither and count as zero.

Calls through a function pointer resolve to the `indirect` rules in
tools/stack_budget.json. A rule names the callers it applies to and the
functions they may reach, so a pointer call reaches only reviewed targets. One
path takes at most `indirect_depth` pointer calls (2 unless the task says
otherwise, an event loop calling a library handler that calls our hook). A
pointer call past that, or one no rule covers, is counted as unresolved and
adds nothing.
Recursion is cut where it closes and flagged, and so are frames GCC marks
dynamic (alloca or a variable length array). Both mean the figure is not a
bound, so a flagged function in our code needs a reviewed depth cap or an
iterative rewrite.

The worst case excludes the interrupt context frame and register window
spills that FreeRTOS keeps on a task stack, see `reserve_bytes` in the budget
file. It is a static estimate, a bench high water mark (`parquet mem`) is the
measured counterpart.

Usage:
  pixi run stack-budget                         # every image with a build
  pixi run stack-budget --image cores3/bringup --path
  pixi run stack-budget --check --image waveshare/logger   # CI, fails past budget
  pixi run stack-budget --write-budgets         # after a reviewed change
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import math
import re
import struct
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BUDGETS = ROOT / "tools" / "stack_budget.json"
INDIRECT = "__indirect_call"

NODE_RE = re.compile(r'^node: \{ title: "([^"]*)" label: "([^"]*)"')
EDGE_RE = re.compile(r'^edge: \{ sourcename: "([^"]*)" targetname: "([^"]*)"')
FRAME_RE = re.compile(r"(\d+) bytes \(([a-z,]+)\)")
FUNC_RE = re.compile(r"^([0-9a-f]{8}) <([^>]+)>:$")
ENTRY_RE = re.compile(r"\tentry\ta1, (\d+)")
# A real call targets a function start. A target with an offset is a literal
# pool word decoded as an instruction, so it is ignored.
CALL_RE = re.compile(r"\tcall(?:0|4|8|12)\t[0-9a-f]+ <([^>+]+)>$")
CALLX_RE = re.compile(r"\tcallx(?:0|4|8|12)\t")


class BudgetError(RuntimeError):
    pass


@dataclass
class Function:
    name: str  # demangled signature, or the symbol for disassembled code
    location: str = ""
    frame: int = 0
    qualifier: str = "static"  # static, dynamic or dynamic,bounded
    source: str = "ci"  # ci, disassembly or unknown
    calls: set[str] = field(default_factory=set)
    indirect: bool = False


@dataclass
class Graph:
    functions: dict[str, Function] = field(default_factory=dict)

    def resolve(self, title: str) -> str:
        return title if title in self.functions else self._unknown(title)

    def _unknown(self, title: str) -> str:
        self.functions[title] = Function(name=title, source="unknown")
        return title


def short_path(text: str) -> str:
    for prefix in (str(ROOT) + "/",):
        text = text.replace(prefix, "")
    return re.sub(r"^.*/esp-idf-v[0-9.]+/", "esp-idf/", text)


def merge(graph: Graph, title: str, function: Function) -> None:
    existing = graph.functions.get(title)
    if existing is None:
        graph.functions[title] = function
        return
    # A COMDAT or inline function compiled in several objects. Keep the
    # largest frame and every call any copy makes.
    if function.frame > existing.frame:
        existing.frame, existing.qualifier, existing.location = (
            function.frame, function.qualifier, function.location)
    existing.calls |= function.calls
    existing.indirect |= function.indirect


def load_ci(paths: list[Path]) -> Graph:
    graph = Graph()
    for path in paths:
        defined: dict[str, Function] = {}
        edges: list[tuple[str, str]] = []
        for line in path.read_text(errors="replace").splitlines():
            if match := NODE_RE.match(line):
                title, label = match.groups()
                parts = label.split("\\n")
                frame = FRAME_RE.search(parts[-1]) if len(parts) >= 3 else None
                if frame:
                    # GCC prints a variadic method's name as ")", so keep the symbol.
                    name = parts[0] if parts[0].strip(") ") else title.rsplit(":", 1)[-1]
                    defined[title] = Function(
                        name=name, location=short_path(parts[1]),
                        frame=int(frame.group(1)), qualifier=frame.group(2))
            elif match := EDGE_RE.match(line):
                edges.append(match.groups())
        for source, target in edges:
            function = defined.get(source)
            if function is None:
                continue
            if target == INDIRECT:
                function.indirect = True
            else:
                function.calls.add(target)
        for title, function in defined.items():
            merge(graph, title, function)
    return graph


def add_disassembly(graph: Graph, listing: str) -> None:
    """Frames and calls for linked code that has no .ci, from `objdump -d`."""
    # A static function's .ci title is `path:symbol`, its ELF symbol is bare.
    known = set(graph.functions) | {title.rsplit(":", 1)[1] for title in graph.functions if ":" in title}
    current: Function | None = None
    frame_seen = False
    for line in listing.splitlines():
        if match := FUNC_RE.match(line):
            name = match.group(2)
            current = None if name in known else Function(name=name, source="disassembly")
            if current is not None:
                graph.functions[name] = current
            frame_seen = False
            continue
        if current is None:
            continue
        if not frame_seen and (match := ENTRY_RE.search(line)):
            current.frame = int(match.group(1))
            frame_seen = True
        elif match := CALL_RE.search(line):
            current.calls.add(match.group(1))
        elif CALLX_RE.search(line):
            current.indirect = True


def pattern_matches(graph: Graph, pattern: str) -> list[str]:
    """A pattern is an fnmatch glob over the demangled signature or the symbol.
    `path-glob::glob` also requires the definition to sit in a matching file."""
    path_glob = None
    if "::" in pattern and not pattern.startswith("*") and "/" in pattern.split("::", 1)[0]:
        path_glob, pattern = pattern.split("::", 1)
    found = []
    for title, function in graph.functions.items():
        if function.source == "unknown":
            continue
        if path_glob and not fnmatch.fnmatchcase(function.location.split(":", 1)[0], path_glob):
            continue
        if fnmatch.fnmatchcase(function.name, pattern) or fnmatch.fnmatchcase(title, pattern):
            found.append(title)
    return sorted(found)


@dataclass
class TaskResult:
    worst: int
    path: list[tuple[str, int]]
    recursion: list[list[str]]
    pointer_cycles: int
    dynamic: list[str]
    unknown: list[str]
    unresolved_indirect: int


IndirectRule = tuple[frozenset[str] | None, list[str]]


def indirect_rules(graph: Graph, specs: list) -> list[IndirectRule]:
    """A plain pattern is a target any pointer call may reach. An object
    `{"from": [...], "to": [...]}` limits its targets to pointer calls made by
    the matching callers, so a library's own callbacks stay out of our paths."""
    rules: list[IndirectRule] = []
    loose: set[str] = set()
    for spec in specs:
        if isinstance(spec, str):
            loose.update(pattern_matches(graph, spec))
            continue
        callers = frozenset(title for pattern in spec["from"] for title in pattern_matches(graph, pattern))
        targets = sorted({title for pattern in spec["to"] for title in pattern_matches(graph, pattern)})
        rules.append((callers, targets))
    if loose:
        rules.insert(0, (None, sorted(loose)))
    return rules


def analyse(graph: Graph, entries: list[str], indirect: list[IndirectRule], depth: int) -> TaskResult:
    sys.setrecursionlimit(max(sys.getrecursionlimit(), 20000))
    # A key is a function and the pointer calls already taken on the path to
    # it. Without the cap every hook could reach every other hook and the
    # worst case would be a chain no event loop can run.
    Key = tuple[str, int]
    memo: dict[Key, tuple[int, Key | None]] = {}
    on_stack: list[Key] = []
    on_stack_titles: set[str] = set()
    cycles: dict[frozenset[str], list[str]] = {}
    pointer_cycles: set[frozenset[str]] = set()
    dynamic: set[str] = set()
    unknown: set[str] = set()
    unresolved: set[str] = set()

    def callees(key: Key) -> list[Key]:
        title, level = key
        function = graph.functions[title]
        out = [(graph.resolve(target), level) for target in sorted(function.calls)]
        if function.indirect:
            targets = sorted({target for callers, rule_targets in indirect
                              if callers is None or title in callers for target in rule_targets})
            if targets and level < depth:
                out += [(target, level + 1) for target in targets]
            else:
                unresolved.add(title)
        return out

    def visit(key: Key) -> int:
        if key in memo:
            return memo[key][0]
        function = graph.functions[key[0]]
        if function.source == "unknown":
            unknown.add(function.name)
        if function.qualifier.startswith("dynamic"):
            dynamic.add(f"{display(function)} ({function.qualifier}, {function.frame} B)")
        on_stack.append(key)
        on_stack_titles.add(key[0])
        best, best_key = 0, None
        for child in callees(key):
            # A function already on the path closes a cycle. Direct recursion
            # is listed. A cycle closed through a pointer call is usually the
            # pattern over-approximation (a library calling back into itself
            # through a hook), so it is only counted.
            if child[0] in on_stack_titles:
                start = next(i for i, (name, _) in enumerate(on_stack) if name == child[0])
                names = sorted({display(graph.functions[name]) for name, _ in on_stack[start:]})
                if on_stack[start] == child:
                    cycles.setdefault(frozenset(names), names)
                else:
                    pointer_cycles.add(frozenset(names))
                continue
            cost = visit(child)
            if cost > best:
                best, best_key = cost, child
        on_stack.pop()
        on_stack_titles.discard(key[0])
        total = function.frame + best
        memo[key] = (total, best_key)
        return total

    worst, start = 0, None
    for entry in entries:
        cost = visit((entry, 0))
        if cost > worst or start is None:
            worst, start = cost, (entry, 0)
    path = []
    key = start
    while key is not None:
        function = graph.functions[key[0]]
        path.append((display(function), function.frame))
        key = memo[key][1]
    return TaskResult(worst, path, sorted(cycles.values()), len(pointer_cycles), sorted(dynamic), sorted(unknown),
                      len(unresolved))


def display(function: Function) -> str:
    name = function.name
    if function.location:
        name += f" [{function.location.rsplit(':', 1)[0]}]"
    return name


def elf_sections(path: Path) -> dict[str, int]:
    data = path.read_bytes()
    if data[:6] != b"\x7fELF\x01\x01":
        raise BudgetError(f"{path} is not a 32-bit little-endian ELF")
    offset = struct.unpack_from("<I", data, 0x20)[0]
    size, count, names_index = struct.unpack_from("<HHH", data, 0x2E)
    headers = [struct.unpack_from("<10I", data, offset + index * size) for index in range(count)]
    names = headers[names_index][4]
    sections = {}
    for header in headers:
        if header[2] & 2 and header[5]:  # SHF_ALLOC and not empty
            start = names + header[0]
            sections[data[start:data.index(b"\0", start)].decode()] = header[5]
    return sections


def sdkconfig(build: Path) -> dict[str, str]:
    values = {}
    for line in (build / "sdkconfig").read_text().splitlines():
        if line.startswith("CONFIG_") and "=" in line:
            key, value = line.split("=", 1)
            values[key] = value.strip('"')
    return values


def objdump_path(build: Path) -> Path | None:
    cache = build / "CMakeCache.txt"
    if cache.is_file():
        for line in cache.read_text().splitlines():
            if line.startswith("CMAKE_OBJDUMP:FILEPATH="):
                tool = Path(line.split("=", 1)[1])
                return tool if tool.is_file() else None
    return None


@dataclass
class ImageReport:
    name: str
    tasks: dict[str, dict]
    sections: dict[str, int]
    stack_memory: list[str] = field(default_factory=list)


# NVS and SPI flash writes disable the cache, so a task that writes NVS must
# run on an internal RAM stack. xTaskCreate always allocates its stack from
# internal RAM. CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY (an ESP-IDF default
# with PSRAM) only lets the calls below take a PSRAM stack, so any of them in
# repo firmware fails the check until it is reviewed.
EXTERNAL_STACK_CONFIG = "CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY"
EXTERNAL_STACK_RE = re.compile(r"\b(xTaskCreateWithCaps|xTaskCreatePinnedToCoreWithCaps|xTaskCreateStatic"
                               r"|xTaskCreateStaticPinnedToCore|xTaskCreateRestrictedStatic)\s*\(")
SOURCE_SKIP = {"build", "managed_components", "vendor"}


def external_stack_sources(root: Path) -> list[str]:
    found = []
    for path in sorted((root / "firmware").rglob("*")):
        if path.suffix not in {".c", ".cc", ".cpp", ".h", ".hpp"} or SOURCE_SKIP & set(path.parts):
            continue
        for number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
            if EXTERNAL_STACK_RE.search(line):
                found.append(f"{path.relative_to(root)}:{number}")
    return found


def stack_memory_findings(config: dict[str, str], sources: list[str]) -> list[str]:
    if config.get(EXTERNAL_STACK_CONFIG) != "y":
        return []  # A PSRAM stack buffer would fail at task creation instead.
    return [f"{where} creates a task with a caller-chosen stack, check it is internal RAM" for where in sources]


def measure_image(name: str, spec: dict, budgets: dict, require_objdump: bool,
                  sources: list[str] | None = None) -> ImageReport:
    build = ROOT / spec["build"]
    description = build / "project_description.json"
    if not description.is_file():
        raise BudgetError(f"{name}: no build in {spec['build']}, run the board build first")
    elf = build / json.loads(description.read_text())["app_elf"]
    ci_files = sorted(build.rglob("*.ci"))
    if not ci_files:
        raise BudgetError(f"{name}: no .ci call graphs in {spec['build']}, rebuild with aq_enable_stack_usage()")
    graph = load_ci(ci_files)
    tool = objdump_path(build)
    if tool is None:
        if require_objdump:
            raise BudgetError(f"{name}: the toolchain objdump named in CMakeCache.txt is missing")
        print(f"warning: {name}: no objdump, prebuilt library frames count as zero", file=sys.stderr)
    else:
        listing = subprocess.run([str(tool), "-d", "--no-show-raw-insn", str(elf)], check=True,
                                 capture_output=True, text=True).stdout
        add_disassembly(graph, listing)
    config = sdkconfig(build)
    tasks = {}
    for task_name, image_task in spec["tasks"].items():
        task = budgets["tasks"][task_name]
        entries = [title for pattern in image_task.get("entry", task.get("entry", []))
                   for title in pattern_matches(graph, pattern)]
        if not entries:
            raise BudgetError(f"{name}: no function matches the {task_name} entry {task['entry']}")
        indirect = indirect_rules(graph, budgets.get("shared_indirect", []) + task.get("indirect", [])
                                  + image_task.get("indirect", []))
        result = analyse(graph, entries, indirect, int(task.get("indirect_depth", 2)))
        stack = image_task.get("stack_bytes")
        if stack is None and "stack_sdkconfig" in task:
            stack = int(config.get(task["stack_sdkconfig"], "0")) or None
        tasks[task_name] = {"result": result, "stack": stack, "indirect_targets": len({t for _, targets in indirect for t in targets})}
    sections = elf_sections(elf)
    return ImageReport(name, tasks, sections, stack_memory_findings(config, sources or []))


def headroom(value: int, percent: float, step: int) -> int:
    return int(math.ceil(value * (1 + percent / 100) / step) * step)


def kib(value: int) -> str:
    return f"{value:,}"


def render(report: ImageReport, spec: dict, budgets: dict, show_path: bool) -> tuple[list[str], list[str]]:
    reserve = int(budgets["policy"]["reserve_bytes"])
    lines = [f"## {report.name}", "",
             "| Task | Stack | Static worst | Plus reserve | Margin | Budget | Flags |",
             "| --- | ---: | ---: | ---: | ---: | ---: | --- |"]
    failures = []
    for task_name, measured in report.tasks.items():
        result: TaskResult = measured["result"]
        stack = measured["stack"]
        budget = spec["tasks"][task_name].get("budget")
        flags = []
        if result.recursion:
            flags.append(f"recursion x{len(result.recursion)}")
        if result.pointer_cycles:
            flags.append(f"pointer cycle x{result.pointer_cycles}")
        if any("(dynamic,bounded" not in item for item in result.dynamic):
            flags.append("unbounded frame")
        elif result.dynamic:
            flags.append("bounded dynamic frame")
        if result.unresolved_indirect:
            flags.append(f"unresolved indirect x{result.unresolved_indirect}")
        with_reserve = result.worst + reserve
        margin = "" if stack is None else kib(stack - with_reserve)
        if stack is not None and with_reserve > stack:
            flags.append("OVER STACK")
            failures.append(f"{report.name} {task_name}: static worst {result.worst} B plus the "
                            f"{reserve} B reserve is past the {stack} B stack")
        if budget is None:
            flags.append("no budget")
        elif result.worst > budget:
            flags.append("OVER BUDGET")
            failures.append(f"{report.name} {task_name}: static worst {result.worst} B is past "
                            f"its budget {budget} B")
        lines.append(f"| {task_name} | {'' if stack is None else kib(stack)} | {kib(result.worst)} | "
                     f"{kib(with_reserve)} | {margin} | {'' if budget is None else kib(budget)} | "
                     f"{', '.join(flags)} |")
    if report.stack_memory:
        failures += [f"{report.name} internal stack: {item}" for item in report.stack_memory]
        lines += ["", *[f"Internal stack check failed. {item}." for item in report.stack_memory]]
    else:
        lines += ["", "Internal stack check passed. Repo firmware creates every task with xTaskCreate, so "
                      "every task stack (the NVS writers included) is internal RAM."]
    lines += ["", "| Section | Bytes | Budget |", "| --- | ---: | ---: |"]
    for section, budget in spec.get("sections", {}).items():
        used = report.sections.get(section, 0)
        mark = ""
        if used > budget:
            mark = " OVER BUDGET"
            failures.append(f"{report.name} {section}: {used} B is past its budget {budget} B")
        lines.append(f"| {section} | {kib(used)} | {kib(budget)}{mark} |")
    for task_name, measured in report.tasks.items():
        result = measured["result"]
        details = []
        if show_path:
            details += [f"    {frame:>6}  {name}" for name, frame in result.path]
        details += [f"    recursion: {' -> '.join(cycle)}" for cycle in result.recursion]
        details += [f"    dynamic frame: {item}" for item in result.dynamic]
        if show_path and result.unknown:
            details.append(f"    no frame data ({len(result.unknown)}): {', '.join(result.unknown[:12])}"
                           + (" ..." if len(result.unknown) > 12 else ""))
        if details:
            lines += ["", f"{task_name}, {measured['indirect_targets']} indirect targets, "
                          f"{len(result.path)} frames deep", "", "```", *details, "```"]
    return lines, failures


def write_budgets(path: Path, budgets: dict, reports: list[ImageReport]) -> None:
    policy = budgets["policy"]
    reserve = int(policy["reserve_bytes"])
    for report in reports:
        spec = budgets["images"][report.name]
        for task_name, measured in report.tasks.items():
            worst = measured["result"].worst
            budget = headroom(worst, policy["stack_headroom_percent"], policy["stack_round_bytes"])
            # Headroom never reaches past the task stack minus the reserve, so a
            # task that is already tight keeps the little room it has.
            if measured["stack"] is not None:
                budget = max(worst, min(budget, measured["stack"] - reserve))
            spec["tasks"][task_name]["measured"] = worst
            spec["tasks"][task_name]["budget"] = budget
        spec["sections_measured"] = {name: report.sections.get(name, 0) for name in spec["sections"]}
        spec["sections"] = {name: headroom(report.sections.get(name, 0), policy["section_headroom_percent"],
                                           policy["section_round_bytes"]) for name in spec["sections"]}
    path.write_text(json.dumps(budgets, indent=2) + "\n")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--image", action="append", help="board/variant, repeatable (default every built image)")
    parser.add_argument("--budgets", type=Path, default=DEFAULT_BUDGETS)
    parser.add_argument("--check", action="store_true", help="exit 1 when a task or section is past its budget")
    parser.add_argument("--write-budgets", action="store_true",
                        help="rewrite measured values and budgets with the policy headroom")
    parser.add_argument("--path", action="store_true", help="print each task's deepest call path")
    args = parser.parse_args(argv)
    budgets = json.loads(args.budgets.read_text())
    names = args.image or [name for name, spec in budgets["images"].items()
                           if (ROOT / spec["build"] / "project_description.json").is_file()]
    if not names:
        print("error: no board build found, run cores3-build or waveshare-build first", file=sys.stderr)
        return 2
    reports, failures, output = [], [], []
    sources = external_stack_sources(ROOT)
    try:
        for name in names:
            if name not in budgets["images"]:
                raise BudgetError(f"unknown image {name}, known: {', '.join(budgets['images'])}")
            spec = budgets["images"][name]
            report = measure_image(name, spec, budgets, require_objdump=args.check or args.write_budgets,
                                   sources=sources)
            reports.append(report)
            lines, image_failures = render(report, spec, budgets, args.path)
            output += lines + [""]
            failures += image_failures
    except BudgetError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    policy = budgets["policy"]
    print(f"# Stack and section budgets\n\nStatic worst case in bytes from GCC call graphs. "
          f"Plus reserve adds {policy['reserve_bytes']} B for the interrupt frame and window spills. "
          f"Budgets allow {policy['stack_headroom_percent']} % on stacks and "
          f"{policy['section_headroom_percent']} % on sections over the committed measurement.\n")
    print("\n".join(output))
    if args.write_budgets:
        write_budgets(args.budgets, budgets, reports)
        print(f"wrote {args.budgets.relative_to(ROOT) if args.budgets.is_relative_to(ROOT) else args.budgets}")
        return 0
    if failures:
        print("Budget check failed. If the growth is intended, review it and run "
              "`pixi run stack-budget --write-budgets` for the affected image.", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1 if args.check else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
