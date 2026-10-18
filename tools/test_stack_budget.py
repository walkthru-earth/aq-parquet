"""Host tests for stack_budget.py on synthetic GCC call graphs and a tiny ELF; no SDK build is read."""

from __future__ import annotations

import contextlib
import faulthandler
import io
import json
from pathlib import Path
import stat
import struct
import sys
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

import stack_budget as sb


def node(title: str, name: str, frame: int, qualifier: str = "static", where: str = "src/a.c:1:1") -> str:
    return f'node: {{ title: "{title}" label: "{name}\\n{where}\\n{frame} bytes ({qualifier})" }}'


def edge(source: str, target: str) -> str:
    return f'edge: {{ sourcename: "{source}" targetname: "{target}" label: "" }}'


def write_ci(path: Path, lines: list[str]) -> Path:
    path.write_text("graph: { title: \"x\"\n" + "\n".join(lines) + "\n}\n")
    return path


def tiny_elf(path: Path, sections: dict[str, int]) -> None:
    """A 32-bit little-endian ELF with only section headers, enough for elf_sections()."""
    names = b"\0" + b"".join(name.encode() + b"\0" for name in [*sections, ".shstrtab"])
    header_offset = 52 + len(names)
    headers = [struct.pack("<10I", *([0] * 10))]
    cursor = 1
    for name, size in sections.items():
        headers.append(struct.pack("<10I", cursor, 1, 2 | 4, 0, 0, size, 0, 0, 4, 0))
        cursor += len(name) + 1
    headers.append(struct.pack("<10I", cursor, 3, 0, 0, 52, len(names), 0, 0, 1, 0))
    ident = b"\x7fELF\x01\x01\x01" + b"\0" * 9
    elf = ident + struct.pack("<HHIIIIIHHHHHH", 2, 94, 1, 0, 0, header_offset, 0, 52, 0, 0, 40,
                              len(headers), len(headers) - 1)
    path.write_bytes(elf + names + b"".join(headers))


def check_graph(root: Path) -> None:
    ci = write_ci(root / "a.ci", [
        node("worker", "void worker(void*)", 200),
        node("/src/a.c:helper", "int helper(int)", 64),
        node("walk", "void walk(int)", 96),
        node("vla", "void vla(int)", 40, "dynamic"),
        node("bounded", "void bounded(int)", 24, "dynamic,bounded"),
        node("/src/a.c:_ZN3Log6printfEPKcz", ")", 352),
        node("dispatch", "void dispatch()", 16),
        node("hook_a", "void hook_a()", 500, where="firmware/common/x.cpp:1:1"),
        node("hook_b", "void hook_b()", 900, where="firmware/board/y.cpp:1:1"),
        node("relay", "void relay()", 32),
        edge("worker", "/src/a.c:helper"), edge("worker", "walk"), edge("worker", "dispatch"),
        edge("walk", "walk"),
        edge("/src/a.c:helper", "vla"), edge("/src/a.c:helper", "bounded"),
        edge("/src/a.c:helper", "/src/a.c:_ZN3Log6printfEPKcz"),
        edge("dispatch", "__indirect_call"), edge("hook_a", "__indirect_call"), edge("relay", "__indirect_call"),
    ])
    graph = sb.load_ci([ci])
    # A variadic method GCC prints as ")" keeps its symbol.
    assert graph.functions["/src/a.c:_ZN3Log6printfEPKcz"].name == "_ZN3Log6printfEPKcz"
    assert graph.functions["worker"].calls == {"/src/a.c:helper", "walk", "dispatch"}
    assert graph.functions["dispatch"].indirect and not graph.functions["worker"].indirect

    # Prebuilt code takes its frame from `entry` and its calls from call8.
    sb.add_disassembly(graph, "\n".join([
        "40000000 <blob>:",
        "40000000:\tentry\ta1, 48",
        "40000003:\tcall8\t40000010 <helper>",
        "40000006:\tcall8\t40000020 <pool+0x4>",
        "40000009:\tcallx8\ta8",
        "40000010 <helper>:",
        "40000010:\tentry\ta1, 999",
    ]))
    assert graph.functions["blob"].frame == 48 and graph.functions["blob"].calls == {"helper"}
    assert graph.functions["blob"].indirect
    assert "helper" not in graph.functions, "a static function's bare symbol keeps its .ci frame"

    # No rule, so the pointer call is unresolved and adds nothing.
    result = sb.analyse(graph, ["worker"], [], 2)
    assert result.worst == 200 + 64 + 352, result.worst
    assert [frame for _, frame in result.path] == [200, 64, 352]
    assert result.unresolved_indirect == 1
    assert result.recursion == [["void walk(int) [src/a.c:1]"]], result.recursion
    assert result.pointer_cycles == 0
    assert any("(dynamic, 40 B)" in item for item in result.dynamic)
    assert any("(dynamic,bounded, 24 B)" in item for item in result.dynamic)

    # A scoped rule reaches only from its callers, and the file filter picks hook_a.
    rules = sb.indirect_rules(graph, [{"from": ["dispatch"], "to": ["firmware/common/*::hook_*"]}])
    assert rules == [(frozenset({"dispatch"}), ["hook_a"])]
    result = sb.analyse(graph, ["worker"], rules, 2)
    assert result.worst == 200 + 16 + 500, result.worst
    assert result.unresolved_indirect == 1, "hook_a has a pointer call no rule covers"

    # The depth cap stops a chain of pointer calls.
    chain = sb.indirect_rules(graph, [{"from": ["dispatch"], "to": ["hook_a"]},
                                      {"from": ["hook_a"], "to": ["relay"]},
                                      {"from": ["relay"], "to": ["hook_b"]}])
    assert sb.analyse(graph, ["worker"], chain, 3).worst == 200 + 16 + 500 + 32 + 900
    capped = sb.analyse(graph, ["worker"], chain, 2)
    assert capped.worst == 200 + 16 + 500 + 32 and capped.unresolved_indirect == 1

    # A cycle closed through a pointer call is counted, not listed as recursion.
    loop = sb.indirect_rules(graph, [{"from": ["dispatch"], "to": ["hook_a"]}, {"from": ["hook_a"], "to": ["dispatch"]}])
    looped = sb.analyse(graph, ["dispatch"], loop, 2)
    assert looped.pointer_cycles == 1 and looped.recursion == []

    # A plain string rule applies to every pointer call.
    loose = sb.indirect_rules(graph, ["relay"])
    assert loose == [(None, ["relay"])]


def make_build(root: Path, sections: dict[str, int]) -> tuple[Path, dict]:
    build = root / "build"
    build.mkdir()
    write_ci(build / "main.ci", [
        node("main_task", "void main_task(void*)", 300),
        node("leaf", "void leaf()", 100),
        edge("main_task", "leaf"),
        edge("main_task", "blob"),
    ])
    tiny_elf(build / "app.elf", sections)
    (build / "project_description.json").write_text(json.dumps({"app_elf": "app.elf"}))
    (build / "sdkconfig").write_text("CONFIG_MAIN_STACK=4096\n")
    listing = build / "listing.txt"
    listing.write_text("40000000 <blob>:\n40000000:\tentry\ta1, 160\n")
    objdump = build / "fake-objdump"
    objdump.write_text(f"#!{sys.executable}\nimport sys\nsys.stdout.write(open({str(listing)!r}).read())\n")
    objdump.chmod(objdump.stat().st_mode | stat.S_IXUSR)
    (build / "CMakeCache.txt").write_text(f"CMAKE_OBJDUMP:FILEPATH={objdump}\n")
    budgets = {
        "policy": {"stack_headroom_percent": 10, "stack_round_bytes": 64, "section_headroom_percent": 2,
                   "section_round_bytes": 256, "reserve_bytes": 512},
        "shared_indirect": [],
        "tasks": {"main": {"entry": ["main_task"], "stack_sdkconfig": "CONFIG_MAIN_STACK"},
                  "tight": {"entry": ["main_task"]}},
        "images": {"fake/app": {"build": str(build),
                                "tasks": {"main": {}, "tight": {"stack_bytes": 1000}},
                                "sections": {".flash.text": 0, ".dram0.bss": 0}}},
    }
    return build, budgets


def run(argv: list[str]) -> tuple[int, str, str]:
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = sb.main(argv)
    return code, out.getvalue(), err.getvalue()


def check_cli(root: Path) -> None:
    build, budgets = make_build(root, {".flash.text": 10_000, ".dram0.bss": 300})
    path = root / "budgets.json"
    path.write_text(json.dumps(budgets))

    # Zero section budgets fail the check, and report mode only warns.
    code, out, err = run(["--budgets", str(path), "--check"])
    assert code == 1 and ".flash.text: 10000 B is past its budget 0 B" in err, err
    assert run(["--budgets", str(path)])[0] == 0
    assert "| main | 4,096 | 460 | 972 | 3,124 |" in out, out

    # --write-budgets records headroom, capped at the stack minus the reserve.
    assert run(["--budgets", str(path), "--write-budgets"])[0] == 0
    written = json.loads(path.read_text())["images"]["fake/app"]
    assert written["tasks"]["main"] == {"measured": 460, "budget": 512}
    assert written["tasks"]["tight"] == {"stack_bytes": 1000, "measured": 460, "budget": 488}
    assert written["sections"] == {".flash.text": 10_240, ".dram0.bss": 512}
    assert written["sections_measured"] == {".flash.text": 10_000, ".dram0.bss": 300}
    code, out, err = run(["--budgets", str(path), "--check", "--path"])
    assert code == 0, err
    assert "       300  void main_task(void*)" in out and "       160  blob" in out, out

    # Mutation: a lowered task budget fails, and so does a stack the worst case outgrows.
    mutated = json.loads(path.read_text())
    mutated["images"]["fake/app"]["tasks"]["main"]["budget"] = 400
    mutated["images"]["fake/app"]["tasks"]["tight"]["stack_bytes"] = 900
    path.write_text(json.dumps(mutated))
    code, _, err = run(["--budgets", str(path), "--check"])
    assert code == 1
    assert "main: static worst 460 B is past its budget 400 B" in err, err
    assert "tight: static worst 460 B plus the 512 B reserve is past the 900 B stack" in err, err

    # A missing build or an unknown image is a usage error, not a pass.
    assert run(["--budgets", str(path), "--image", "nope/none"])[0] == 2
    (build / "project_description.json").unlink()
    assert run(["--budgets", str(path), "--image", "fake/app"])[0] == 2


def check_internal_stacks(root: Path) -> None:
    source = root / "firmware" / "common" / "x.cpp"
    source.parent.mkdir(parents=True)
    source.write_text("void a() { xTaskCreate(f, \"ok\", 4096, nullptr, 1, nullptr); }\n")
    skipped = root / "firmware" / "board" / "managed_components" / "lib" / "y.c"
    skipped.parent.mkdir(parents=True)
    skipped.write_text("xTaskCreateWithCaps(f, \"lib\", 4096, 0, 1, 0, MALLOC_CAP_SPIRAM);\n")
    assert sb.external_stack_sources(root) == []
    source.write_text("void a() {\n  xTaskCreateStatic (f, \"psram\", 4096, nullptr, 1, stack, &tcb);\n}\n")
    assert sb.external_stack_sources(root) == ["firmware/common/x.cpp:2"]
    allowed = {sb.EXTERNAL_STACK_CONFIG: "y"}
    findings = sb.stack_memory_findings(allowed, ["firmware/common/x.cpp:2"])
    assert findings and "firmware/common/x.cpp:2" in findings[0]
    assert sb.stack_memory_findings({}, ["firmware/common/x.cpp:2"]) == []
    # The committed firmware only uses xTaskCreate.
    assert sb.external_stack_sources(sb.ROOT) == []


def check_runtime_report() -> None:
    """`parquet mem` is answered on the board task and parquet_device waits for the heap line."""
    source = (sb.ROOT / "firmware" / "common" / "logger" / "src" / "aq_logger.cpp").read_text()
    code = " ".join(source.split())
    assert 'std::strcmp(input.text, "parquet mem") == 0) report_runtime(input.received_mono_us, true);' in code
    # New fields go at the end so a reader of the older lines keeps working.
    assert '"nimble_host=%ld tcpip=%ld uptime_s=%lld esp_timer=%ld sys_evt=%ld\\n"' in code
    assert '"psram_min=%lu psram_largest=%lu\\n"' in code

    import parquet_device
    tasks = "DIAG tasks aq-board=5000 parquet-sd=12000 aq-lan=-1 aq_ble_error=900"
    heap = "DIAG heap internal_free=1 internal_min=1 internal_largest=1 psram_largest=1"
    printed: list[str] = []
    with patch.object(parquet_device, "send_command") as send, \
            patch.object(parquet_device, "lines_until", return_value=iter([tasks, heap, "PARQUET ROW"])), \
            patch("builtins.print", side_effect=lambda line, **_: printed.append(line)):
        assert parquet_device.command(None, SimpleNamespace(text="parquet  mem", timeout=1)) == 0
    send.assert_called_once_with(None, "parquet mem")
    assert printed == [tasks, heap]


def check_committed_budgets() -> None:
    budgets = json.loads(sb.DEFAULT_BUDGETS.read_text())
    policy = budgets["policy"]
    for name, spec in budgets["images"].items():
        for task_name, task in spec["tasks"].items():
            assert task_name in budgets["tasks"], f"{name} names an undefined task {task_name}"
            assert task.get("budget", 0) >= task.get("measured", 0), f"{name} {task_name} budget below measured"
            if "stack_bytes" in task and "budget" in task:
                assert task["budget"] + policy["reserve_bytes"] <= task["stack_bytes"], f"{name} {task_name}"
        for section, budget in spec["sections"].items():
            assert budget >= spec["sections_measured"][section], f"{name} {section}"


def main() -> None:
    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw)
        (root / "graph").mkdir()
        (root / "cli").mkdir()
        (root / "src").mkdir()
        check_graph(root / "graph")
        check_cli(root / "cli")
        check_internal_stacks(root / "src")
    check_runtime_report()
    check_committed_budgets()
    print("stack_budget tests passed")


if __name__ == "__main__":
    faulthandler.dump_traceback_later(120, exit=True)
    main()
