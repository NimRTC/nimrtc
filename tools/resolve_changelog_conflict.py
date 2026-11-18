#!/usr/bin/env python3
"""Resolve the `CHANGELOG.md` conflict between the QoE branch and `main`.

## Why this exists

The QoE branch (`feat/s7-qoe-default-and-freeze-wiring`) was cut before
the `v1.2.0` release restructure, so its `CHANGELOG.md` edit and
`main`'s conflict: `main` regrouped the whole `[1.2.0]` body into the
canonical `Keep a Changelog` order, while the QoE branch appended its
entries under the *old* layout.

Resolving the conflict by hand would mean choosing one side and losing
the other's content.  Choosing `main`'s side loses the QoE release
notes; choosing the QoE branch's side reverts the whole restructure.

The resolution is mechanical, and this script performs it:

- the QoE branch's entries are **extracted** from its side of the
  conflict (`### Changed` with three entries, `### Fixed` with one);
- they are **merged** into `main`'s existing `### Changed` / `### Fixed`
  groups -- the groups `main` already has, at the position the
  canonical order puts them;
- the `136` -> `145` field-count fix is applied.

Nothing is reworded.  The script refuses to run if it cannot find the
blocks it expects, so a silent mis-resolution is not possible.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

CHANGELOG = Path(__file__).resolve().parent.parent / "CHANGELOG.md"


def read() -> list[str]:
    return CHANGELOG.read_bytes().decode("utf-8-sig").split("\n")


def write(lines: list[str]) -> None:
    CHANGELOG.write_bytes("\n".join(lines).encode("utf-8"))


def find_conflict(lines: list[str]) -> tuple[int, int]:
    start = next(i for i, l in enumerate(lines) if l.startswith("<<<<<<<"))
    mid = next(i for i, l in enumerate(lines) if l == "=======")
    end = next(i for i, l in enumerate(lines) if l.startswith(">>>>>>>"))
    return start, mid, end


def group_range(lines: list[str], heading: str, after: int) -> tuple[int, int]:
    """The `[start, end)` range of the `### heading` group after `after`."""
    start = None
    for i in range(after, len(lines)):
        if lines[i].startswith("### "):
            if lines[i][4:].strip() == heading:
                start = i
                break
            if start is not None:
                break
    if start is None:
        raise SystemExit(f"resolve_changelog_conflict: no '### {heading}' group")
    end = len(lines)
    for i in range(start + 1, len(lines)):
        if lines[i].startswith("### ") or lines[i].startswith("## "):
            end = i
            break
    return start, end


def main() -> int:
    lines = read()
    conflict_start, conflict_mid, conflict_end = find_conflict(lines)
    theirs = lines[conflict_mid + 1 : conflict_end]

    # The QoE side contributes one `### Changed` group and one extra
    # `### Fixed` entry.
    changed_at = next(
        i for i, l in enumerate(theirs) if l.strip() == "### Changed"
    )
    fixed_at = next(i for i, l in enumerate(theirs) if l.strip() == "### Fixed")
    added_at = next(
        i for i, l in enumerate(theirs) if l.strip() == "### Added"
    )

    changed_group = theirs[changed_at:fixed_at]
    fixed_entry = theirs[fixed_at:added_at]

    # Drop the conflict and keep `main`'s side.
    lines = (
        lines[:conflict_start] + lines[conflict_end + 1 :]
    )

    # Merge the `### Changed` group into `main`'s `### Changed` group.
    cs, ce = group_range(lines, "Changed", 0)
    # The `### Changed` group on `main` starts with the heading, a blank
    # line and then the entries.  The QoE entries go *first*, because
    # they are the newest change in the cycle.
    insert_at = cs + 2
    lines = lines[:insert_at] + changed_group[2:] + lines[insert_at:]

    # Merge the extra `### Fixed` entry into `main`'s `### Fixed` group.
    fs, _fe = group_range(lines, "Fixed", 0)
    insert_at = fs + 2
    lines = lines[:insert_at] + fixed_entry[2:] + lines[insert_at:]

    # The `136` -> `145` field-count fix.  Two places carry the count:
    # the S8 entry ("lists all **136** fields") and the release status
    # paragraph ("The 136-field table").  The QoE branch's *non*-conflicting
    # hunk already fixed the first one, so either may be absent here --
    # but the script must end with the right number in both places.
    text = "\n".join(lines)
    text, n1 = re.subn(r"all \*\*136\*\* fields", "all **145** fields", text)
    text, n2 = re.subn(r"The 136-field table", "The 145-field table", text)
    if "**136** fields" in text or "The 136-field table" in text:
        raise SystemExit(
            "resolve_changelog_conflict: a 136-field count survived the merge"
        )
    if n1 + n2 == 0:
        raise SystemExit(
            "resolve_changelog_conflict: found no 136-field count to update"
        )

    write(text.split("\n"))
    print(
        "resolve_changelog_conflict: merged the QoE entries into main's "
        "### Changed / ### Fixed groups and applied the 145-field fix"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
