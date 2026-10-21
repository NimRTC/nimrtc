#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
tools/apply_api_freeze_markers.py
=================================
Apply the per-symbol stability markers recommended by the API-FREEZE
scanner (`scripts/api_freeze_scan.js`).

The scanner (`scripts/api_freeze_scan.js`) walks every public header
under `src/<mod>/include/nimrtc/**` and reports, per namespace-scope
declaration, whether a stability marker (`NIMRTC_API_STABLE` /
`NIMRTC_API_UNSTABLE` / `NIMRTC_API_DEPRECATED`) is already present and
which marker the classification policy recommends.  This tool consumes
that report (`build/_api_freeze_symbols.json`) and inserts the missing
markers.

## Insertion rules (by declaration kind)

    class / struct / union   ->  class  NIMRTC_API_STABLE Foo
                                 (marker goes right after the class-key,
                                  which keeps the `[[deprecated]]` expansion
                                  of a future annotation mode valid)
    enum                    ->  enum class NIMRTC_API_STABLE Foo
    using                   ->  using NIMRTC_API_STABLE Foo = ...
    function / constant     ->  <leading [[attr]]> NIMRTC_API_STABLE <decl>

Functions and constants are anchored at the *start of the statement*
(after any leading `[[nodiscard]]`-style attribute specifiers) so that
the marker stays a valid attribute position if `NIMRTC_API_UNSTABLE`
ever starts expanding to `[[deprecated]]`.

## Usage

    python tools/apply_api_freeze_markers.py --dry-run
    python tools/apply_api_freeze_markers.py --apply
    python tools/apply_api_freeze_markers.py --apply --only src/core

Exit codes:
    0  success (or a clean dry run)
    1  one or more symbols could not be annotated (see the report)
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
from collections import defaultdict

ROOT = pathlib.Path(__file__).resolve().parents[1]
REPORT = ROOT / "build" / "_api_freeze_symbols.json"

MARKERS = ("NIMRTC_API_STABLE", "NIMRTC_API_UNSTABLE")

# The scanner reports the short classification names ("STABLE" / "UNSTABLE");
# map them onto the macro names that actually get inserted.
MACRO_OF = {
    "STABLE": "NIMRTC_API_STABLE",
    "UNSTABLE": "NIMRTC_API_UNSTABLE",
    "DEPRECATED": "NIMRTC_API_DEPRECATED",
}


# ---------------------------------------------------------------------------
# Source masking: blank out comments and string literals while preserving
# offsets, so that a symbol name is never matched inside a comment.
# ---------------------------------------------------------------------------

def mask_source(text: str) -> str:
    out = list(text)
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        c2 = text[i:i + 2]
        if c2 == "//":
            j = text.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
            continue
        if c2 == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if text[k] != "\n":
                    out[k] = " "
            i = j
            continue
        if c in "\"'":
            quote = c
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == quote:
                    j += 1
                    break
                j += 1
            for k in range(i, min(j, n)):
                if text[k] != "\n":
                    out[k] = " "
            i = j
            continue
        i += 1
    return "".join(out)


# ---------------------------------------------------------------------------
# Statement anchoring
# ---------------------------------------------------------------------------

# A line that ends the previous statement (or block) and therefore starts
# a new declaration.
def _starts_new_statement(prev: str) -> bool:
    p = prev.strip()
    if not p:
        return True
    if p.endswith((";", "{", "}", ":")):
        return True
    if p.startswith("#"):
        return True
    if p.startswith(("//", "*", "/*")):
        return True
    return False


ATTR_RE = re.compile(r"\[\[[^\]]*\]\]\s*")


def statement_start(masked: str, match_start: int) -> int:
    """Return the offset of the statement that contains `match_start`."""
    line_start = masked.rfind("\n", 0, match_start) + 1
    # Walk upwards to the first line that does not continue the statement.
    pos = line_start
    while pos > 0:
        prev_line_start = masked.rfind("\n", 0, pos - 1) + 1
        prev = masked[prev_line_start:pos - 1]
        if _starts_new_statement(prev):
            break
        pos = prev_line_start
        if pos == 0:
            break
    # Skip leading whitespace and any leading attribute specifiers.
    i = pos
    while i < len(masked) and masked[i] in " \t":
        i += 1
    while True:
        m = ATTR_RE.match(masked, i)
        if not m:
            break
        i = m.end()
    return i


# ---------------------------------------------------------------------------
# Per-kind insertion
# ---------------------------------------------------------------------------

def insert_for_kind(text: str, masked: str, symbol: dict, marker: str):
    """Return (offset, description) or (None, reason-for-failure)."""
    name = symbol["name"]
    kind = symbol["kind"]
    line0 = symbol["line"]  # 1-based line of the declaration start

    if kind in ("class", "struct", "union"):
        pat = re.compile(r"\b(class|struct|union)(\s+)(%s)\b" % re.escape(name))
    elif kind == "enum":
        pat = re.compile(r"\benum(\s+class)?(\s+)(%s)\b" % re.escape(name))
    elif kind == "using":
        pat = re.compile(r"\busing(\s+)(%s)\s*=" % re.escape(name))
    else:
        if name.startswith("operator"):
            # Operator overloads: the name contains non-word characters, so
            # the plain `\b(name)\b` pattern does not match.  Match the
            # operator token itself and anchor at the statement start.
            pat = re.compile(r"operator\s*%s" % re.escape(name[len("operator"):]))
        else:
            pat = re.compile(r"\b(%s)\b" % re.escape(name))

    # Candidate offsets: prefer the line the scanner reported.
    candidates = [m for m in pat.finditer(masked)]
    if not candidates:
        return None, "declaration not found"

    def line_of(pos: int) -> int:
        return masked.count("\n", 0, pos) + 1

    exact = [m for m in candidates if abs(line_of(m.start()) - line0) <= 2]
    if not exact:
        # Fall back to the first candidate that is not already annotated.
        exact = [m for m in candidates
                 if not masked[max(0, m.start() - 40):m.start()].rstrip().endswith(
                     ("STABLE", "UNSTABLE", "DEPRECATED"))]
    if not exact:
        return None, "no unambiguous declaration site"

    m = exact[0]
    if kind in ("class", "struct", "union"):
        # groups: 1=keyword, 2=space, 3=name
        return m.start(3), f"{kind} {name}"
    if kind == "enum":
        # groups: 1='enum class' or None, 2=space, 3=name
        return m.start(3), f"enum {name}"
    if kind == "using":
        # groups: 1=space, 2=name
        return m.start(2), f"using {name}"

    # function / constant: anchor at the start of the statement.
    offset = statement_start(masked, m.start())
    return offset, f"{kind} {name}"


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--apply", action="store_true",
                    help="write the changes (default: dry run)")
    ap.add_argument("--only", default="",
                    help="limit to a subtree prefix, e.g. src/core")
    ap.add_argument("--report", default=str(REPORT),
                    help="path to _api_freeze_symbols.json")
    args = ap.parse_args()

    report_path = pathlib.Path(args.report)
    if not report_path.exists():
        print(f"ERROR: scanner report not found: {report_path}\n"
              f"       run: node scripts/api_freeze_scan.js", file=sys.stderr)
        return 2

    data = json.loads(report_path.read_text(encoding="utf-8"))

    todo = []
    for s in data["symbols"]:
        if s["markers"]:
            continue
        if s.get("recommend") == "NONE":
            continue
        if args.only and not s["file"].startswith(args.only):
            continue
        todo.append(s)

    by_file: dict[str, list[dict]] = defaultdict(list)
    for s in todo:
        by_file[s["file"]].append(s)

    changed_files = 0
    inserted = 0
    failures: list[str] = []

    for rel, syms in sorted(by_file.items()):
        path = ROOT / rel
        raw = path.read_bytes()
        bom = raw.startswith(b"\xef\xbb\xbf")
        text = raw.decode("utf-8-sig")
        masked = mask_source(text)

        # Collect insertion points, then apply bottom-up so offsets stay valid.
        points = []
        for s in syms:
            marker = MACRO_OF.get(s["recommend"], s["recommend"])
            offset, desc = insert_for_kind(text, masked, s, marker)
            if offset is None:
                failures.append(f"{rel}:{s['line']}  {desc}  ({s['kind']} {s['name']})")
                continue
            # Skip if a marker is already immediately in front.
            before = masked[max(0, offset - 60):offset]
            if re.search(r"NIMRTC_API_(STABLE|UNSTABLE|DEPRECATED)\s*$", before):
                continue
            points.append((offset, marker, desc))

        if not points:
            continue

        # De-duplicate identical offsets (two symbols on the same statement).
        seen: dict[int, tuple[str, str]] = {}
        for offset, marker, desc in points:
            if offset in seen:
                failures.append(f"{rel}: duplicate insertion point at offset {offset} "
                                f"({desc} vs {seen[offset][1]})")
                continue
            seen[offset] = (marker, desc)

        for offset in sorted(seen, reverse=True):
            marker = seen[offset][0]
            text = text[:offset] + marker + " " + text[offset:]
            inserted += 1

        if args.apply:
            out = text.encode("utf-8")
            if bom:
                out = b"\xef\xbb\xbf" + out
            path.write_bytes(out)
            changed_files += 1
        else:
            print(f"[dry-run] {rel}: {len(seen)} marker(s)")

    print(f"\nfiles touched   : {changed_files}")
    print(f"markers inserted: {inserted}")
    if failures:
        print(f"\n{len(failures)} symbol(s) need manual attention:")
        for f in failures:
            print("  " + f)
    if not args.apply:
        print("\n(dry run -- pass --apply to write the changes)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
