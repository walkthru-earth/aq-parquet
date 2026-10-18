"""Host tests for size_report.py on crafted `idf.py size --format json2` reports."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile

SCRIPT = Path(__file__).resolve().with_name("size_report.py")


def report(total: int, **regions: int) -> str:
    return json.dumps({"version": "1.2", "total_size": total,
                       "layout": [{"name": name, "used": used} for name, used in regions.items()]})


def run(*arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run([sys.executable, str(SCRIPT), "--label", "cores3 (bringup)", *arguments],
                          capture_output=True, text=True, timeout=60, check=False)


def main() -> None:
    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw)
        current = root / "size.json"
        current.write_text(report(1_000, IRAM=200, DRAM=100))

        result = run(str(current))
        assert result.returncode == 0, result.stderr
        assert "No baseline build was available" in result.stdout
        assert "| Total image | 1,000 |" in result.stdout

        good = root / "good.json"
        good.write_text(report(900, IRAM=200))
        result = run(str(current), "--baseline", str(good), "--baseline-from", "main abc")
        assert result.returncode == 0, result.stderr
        assert "relative to the main abc" in result.stdout
        assert "| Total image | 1,000 | +100 (+11.11%) |" in result.stdout
        assert "| IRAM | 200 | 200 | 0 |" not in result.stdout and "| IRAM | 200 | 0 |" in result.stdout
        assert "| DRAM | 100 | new |" in result.stdout

        # A broken baseline must not fail a green build leg. Each case falls
        # back to absolute sizes and says why on stdout and stderr.
        malformed = {
            "not json": "{truncated",
            "empty file": "",
            "json list": "[]",
            "no total": json.dumps({"layout": []}),
            "no layout": json.dumps({"total_size": 5}),
            "text total": json.dumps({"total_size": "big", "layout": []}),
            "layout not a list": json.dumps({"total_size": 5, "layout": 7}),
            "region without used": json.dumps({"total_size": 5, "layout": [{"name": "IRAM"}]}),
            "region not an object": json.dumps({"total_size": 5, "layout": ["IRAM"]}),
        }
        for name, text in malformed.items():
            broken = root / "broken.json"
            broken.write_text(text)
            result = run(str(current), "--baseline", str(broken), "--baseline-from", "main abc")
            assert result.returncode == 0, f"{name}: {result.stderr}"
            assert "| Total image | 1,000 |" in result.stdout, name
            assert "| Change |" not in result.stdout, name
            assert "baseline" in result.stdout and "could not be read" in result.stdout, name
            assert "warning" in result.stderr.lower(), name

        result = run(str(current), "--baseline", str(root / "missing.json"))
        assert result.returncode == 0 and "could not be read" in result.stdout, result.stderr

        # The current report is this build's own output, so a broken one
        # still fails the step.
        current.write_text("{truncated")
        result = run(str(current))
        assert result.returncode != 0, result.stdout

    print("size_report.py host tests passed")


if __name__ == "__main__":
    main()
