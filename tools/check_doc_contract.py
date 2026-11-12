#!/usr/bin/env python3
"""Doc-contract check for the plugin catalogue.

`docs/plugin_author_guide.md` §4 is the table a plugin author reads to
find out which families they can replace, which `EngineConfig` knob
selects theirs, and which registration macro they call.  Every cell in
that table is a *promise about the code*, and a promise that silently
drifts is worse than no promise at all: a typo'd macro name sends an
author to a compiler error, and a knob that no longer exists sends them
to a runtime failure.

This script is the guard.  It parses the §4 table and checks, in both
directions:

  * **Knob → code.**  Every "Engine config knob" cell names a field
    that must exist in `src/engine/include/nimrtc/engine/engine.hpp`.
    A knob the engine does not have is a knob the author cannot set.
  * **Macro → code.**  Every "Factory macro" cell names a macro that
    must be defined in `src/core/include/nimrtc/core/registry.hpp`.
    A macro the registry does not define is a macro that will not
    compile.
  * **Code → doc.**  Every `NIMRTC_REGISTER_*` macro the registry
    defines must appear in the table.  A macro the guide does not list
    is a seam the guide forgot — the exact B4/B5 failure mode.

Usage:

    python tools/check_doc_contract.py [--repo-root ROOT]

Exit code 0 when every check passes, 1 otherwise.  The output is a
list of human-readable problems, one per line.

Design note: this is deliberately a *text* check, not a C++ reflection
check.  The doc table is markdown; the code is C++ headers.  A regex
over the headers is enough to catch the class of drift we care about
(a renamed field, a renamed macro) without needing a compiler in the
loop, which is what makes the check cheap enough to run on every CI
push.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# the section-4 catalogue table lives between these two markers in the guide.
TABLE_START = "## 4. Interface catalogue"
TABLE_END = "## 5. The lifecycle contract"

ENGINE_HEADER = Path("src/engine/include/nimrtc/engine/engine.hpp")
REGISTRY_HEADER = Path("src/core/include/nimrtc/core/registry.hpp")
PLUGIN_GUIDE = Path("docs/plugin_author_guide.md")

# A macro cell looks like `NIMRTC_REGISTER_VIDEO_SENDER` — possibly
# several, comma separated, possibly with a trailing note in parens.
MACRO_RE = re.compile(r"NIMRTC_REGISTER_[A-Z0-9_]+")

# A knob cell looks like `video_sender_name` — a C++ identifier.
KNOB_RE = re.compile(r"\b([a-z][a-z0-9_]*_name)\b")

# `EngineConfig` field declarations.  They look like:
#     std::string video_sender_name = "reference";
#     std::string_view transport_name = "ice";
FIELD_RE = re.compile(
    r"^\s*std::string(?:_view)?\s+([a-z][a-z0-9_]*)\s*(?:=|\{|;)",
    re.MULTILINE,
)

# `NIMRTC_REGISTER_*` macro definitions in the registry header.
MACRO_DEF_RE = re.compile(r"^\s*#define\s+(NIMRTC_REGISTER_[A-Z0-9_]+)",
                          re.MULTILINE)


class Problem:
    """One contract violation, with enough context to fix it."""

    def __init__(self, kind: str, detail: str) -> None:
        self.kind = kind
        self.detail = detail

    def __str__(self) -> str:
        return f"[{self.kind}] {self.detail}"


def read(path: Path, root: Path) -> str:
    full = root / path
    if not full.is_file():
        raise SystemExit(f"error: {path} not found under {root}")
    return full.read_text(encoding="utf-8")


def extract_table(guide: str) -> str:
    """Return the section-4 catalogue table body (the markdown rows)."""
    start = guide.find(TABLE_START)
    if start < 0:
        raise SystemExit(f"error: '{TABLE_START}' not found in the guide")
    end = guide.find(TABLE_END, start)
    if end < 0:
        # The table runs to the end of the section; use the next heading.
        end = guide.find("\n## ", start + len(TABLE_START))
        if end < 0:
            end = len(guide)
    return guide[start:end]


def table_rows(table: str) -> list[str]:
    """Return the markdown table rows (skipping the header + separator)."""
    rows = []
    for line in table.splitlines():
        stripped = line.strip()
        if not stripped.startswith("|"):
            continue
        # Skip the separator row (`|---|---|`) and the header row.
        if set(stripped) <= set("|-: "):
            continue
        rows.append(stripped)
    # The first row is the column header.
    return rows[1:] if rows else []


def split_cells(row: str) -> list[str]:
    """Split a markdown row into its cells."""
    cells = row.strip().strip("|").split("|")
    return [c.strip() for c in cells]


def collect_engine_fields(engine_header: str) -> set[str]:
    return set(FIELD_RE.findall(engine_header))


def collect_registry_macros(registry_header: str) -> set[str]:
    return set(MACRO_DEF_RE.findall(registry_header))


def check(repo_root: Path) -> list[Problem]:
    guide = read(PLUGIN_GUIDE, repo_root)
    engine_header = read(ENGINE_HEADER, repo_root)
    registry_header = read(REGISTRY_HEADER, repo_root)

    engine_fields = collect_engine_fields(engine_header)
    registry_macros = collect_registry_macros(registry_header)

    table = extract_table(guide)
    rows = table_rows(table)

    problems: list[Problem] = []

    documented_macros: set[str] = set()
    documented_knobs: set[str] = set()

    for row in rows:
        cells = split_cells(row)
        if len(cells) < 4:
            problems.append(Problem(
                "malformed-row",
                f"§4 row has {len(cells)} cells, expected at least 4: {row}"))
            continue

        family = cells[0]
        knob_cell = cells[2]
        macro_cell = cells[3]

        # ---- knob → code -------------------------------------------------
        knobs = set(KNOB_RE.findall(knob_cell))
        for knob in sorted(knobs):
            documented_knobs.add(knob)
            if knob not in engine_fields:
                problems.append(Problem(
                    "missing-knob",
                    f"section-4 family '{family}' names EngineConfig knob "
                    f"'{knob}', but engine.hpp declares no such field"))

        # ---- macro → code ------------------------------------------------
        macros = set(MACRO_RE.findall(macro_cell))
        for macro in sorted(macros):
            documented_macros.add(macro)
            if macro not in registry_macros:
                problems.append(Problem(
                    "missing-macro",
                    f"section-4 family '{family}' names factory macro "
                    f"'{macro}', but registry.hpp defines no such macro"))

    # ---- code → doc ------------------------------------------------------
    # Every registered macro must be listed.  The two exceptions are the
    # RTP / SDP macros, which §4.2 explicitly calls out as deprecated
    # and not-pluggable — but the guide still has to *mention* them, so
    # they are not exempt from the check.
    for macro in sorted(registry_macros):
        if macro not in documented_macros:
            problems.append(Problem(
                "undocumented-macro",
                f"registry.hpp defines '{macro}' but the section-4 catalogue "
                f"does not list it — a plugin author would never find it"))

    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=None,
                        help="repository root (default: the parent of tools/)")
    args = parser.parse_args()

    repo_root = args.repo_root or Path(__file__).resolve().parent.parent

    problems = check(repo_root)
    if not problems:
        print("doc contract OK: the section-4 catalogue matches engine.hpp and "
              "registry.hpp")
        return 0

    for p in problems:
        print(p)
    print(f"\n{len(problems)} doc-contract problem(s) found")
    return 1


if __name__ == "__main__":
    sys.exit(main())
