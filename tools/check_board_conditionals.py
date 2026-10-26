"""Fail on a board-name preprocessor conditional in shared firmware (issue #37).

Shared code takes board capabilities from hooks, ServiceConfig and sdkconfig
defaults, never from a board name (AGENTS.md, code boundaries). This check
fails on an #if, #ifdef, #ifndef, #elif, #elifdef or #elifndef whose condition
names a board, a modem or a chip target, also inside a longer macro name such
as CONFIG_OS_BOARD_CORES3. CONFIG_IDF_TARGET_ARCH_* names a CPU family, not a
chip, and stays allowed. Third-party vendor code is skipped.

The condition is read the way the preprocessor reads it. CRLF line ends count
as LF (read_text uses universal newlines), a backslash newline joins lines, and a comment, line or block, counts
as one space, so a name in a trailing comment does not fire and a condition
split by a comment or a continuation still does. String and character text is
not read, so a raw string line is never a directive. Comments and literals use
the shared lexer in stack_budget.py.

Exit 0 when clean, 1 with one path:line per finding, 2 on a usage or read
error. tools/lint.sh fails on anything but 0.

Usage:
  python tools/check_board_conditionals.py [dir ...]   # default firmware/common
"""

from __future__ import annotations

from pathlib import Path
import re
import sys

from stack_budget import LEXEME_RE

ROOT = Path(__file__).resolve().parents[1]
SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp", ".inc"}
SKIP = {"vendor", "build", "managed_components"}
BOARD_NAMES_RE = re.compile(r"CORES3|CORE_S3|M5STACK|M5_?CORE|WAVESHARE|SIM7[0-9]{3}|CONFIG_IDF_TARGET(?:\b|_ESP)",
                            re.I)
CONDITIONAL_RE = re.compile(r"[ \t]*#[ \t]*(?:if|ifdef|ifndef|elif|elifdef|elifndef)\b(.*)")
# The shared lexer plus line splices and line ends, in one pass.
LOGICAL_RE = re.compile(LEXEME_RE.pattern + r"|(?P<splice>\\\n)|(?P<newline>\n)", re.S)


def logical_lines(text: str) -> list[str]:
    """Lines as the preprocessor sees them, one entry per source line. A line
    that absorbed later lines through a splice or a block comment holds all
    of them, and the lines it absorbed are empty, so indexes stay line numbers."""
    pending = 0

    def piece(match: re.Match) -> str:
        nonlocal pending
        token = match.group(0)
        if match.group("newline"):
            ends, pending = "\n" * (1 + pending), 0
            return ends
        if match.group("splice"):
            pending += 1
            return ""
        if match.group("comment"):
            pending += token.count("\n")
            return " "
        if match.group("word"):
            return token
        # A literal keeps only its line ends, so a raw string line such as
        # "#if CORES3" is not a directive.
        pending += token.count("\\\n")
        return re.sub(r"[^\n]", " ", token.replace("\\\n", ""))

    return LOGICAL_RE.sub(piece, text).split("\n")


def findings(paths: list[Path]) -> list[str]:
    found = []
    for base in paths:
        if not base.is_dir():
            raise FileNotFoundError(f"{base} is not a directory")
        for path in sorted(base.rglob("*")):
            if path.suffix not in SUFFIXES or SKIP & set(path.relative_to(base).parts) or not path.is_file():
                continue
            for number, line in enumerate(logical_lines(path.read_text(errors="replace")), 1):
                if (match := CONDITIONAL_RE.match(line)) and BOARD_NAMES_RE.search(match.group(1)):
                    found.append(f"{path}:{number}: {' '.join(line.split())}")
    return found


def main(argv: list[str] | None = None) -> int:
    args = sys.argv[1:] if argv is None else argv
    if any(arg.startswith("-") for arg in args):
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    try:
        found = findings([Path(arg) for arg in args] or [ROOT / "firmware" / "common"])
    except OSError as error:
        print(f"check_board_conditionals: {error}", file=sys.stderr)
        return 2
    for line in found:
        print(line)
    if found:
        print("!!! board-name preprocessor conditional in shared firmware, use hooks or sdkconfig capabilities",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
