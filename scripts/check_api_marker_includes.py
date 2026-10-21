#!/usr/bin/env python3
"""
scripts/check_api_marker_includes.py
====================================
Verify that every public header which *uses* a stability marker macro
(`NIMRTC_API_STABLE` / `NIMRTC_API_UNSTABLE` / `NIMRTC_API_DEPRECATED`)
also `#include`s the header that defines them
(`<nimrtc/core/api_version.hpp>`).

The markers are documented as "visual only" and expand to nothing in
default mode, so a header can *use* them without seeing the definition
when the marker lands in a header that is also compiled as the first
include in a translation unit — the resulting MSVC error is an
obscure `error C2079: uses undefined struct "NIMRTC_API_STABLE"`
cascade rather than a clean "undeclared identifier".

Usage:
    python scripts/check_api_marker_includes.py            # list offenders
    python scripts/check_api_marker_includes.py --fix      # insert include
    python scripts/check_api_marker_includes.py --path src # limit subtree

Exit codes:
    0  every marker-using header includes api_version.hpp (or --fix fixed them)
    1  offenders remain
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

MARKER_RE = re.compile(r"\bNIMRTC_API_(?:STABLE|UNSTABLE|DEPRECATED)\b")
INCLUDE_RE = re.compile(r"#\s*include\s*[<\"]nimrtc/core/api_version\.hpp[>\"]")
ANY_INCLUDE_RE = re.compile(r"^#\s*include\s*([<\"])([^>\"]+)[>\"]", re.MULTILINE)

SKIP_DIR_PARTS = {"third_party", "build", ".git"}


def find_offenders(root: pathlib.Path) -> list[pathlib.Path]:
    offenders: list[pathlib.Path] = []
    for path in sorted(root.glob("**/include/**/*.hpp")):
        rel_parts = path.relative_to(root).parts
        if any(part in SKIP_DIR_PARTS for part in rel_parts):
            continue
        text = path.read_text(encoding="utf-8-sig", errors="replace")
        if not MARKER_RE.search(text):
            continue
        if INCLUDE_RE.search(text):
            continue
        # Allow a relative include of the same file name too.
        if "api_version.hpp" in text:
            continue
        offenders.append(path)
    return offenders


def insert_include(path: pathlib.Path) -> bool:
    """Insert `#include <nimrtc/core/api_version.hpp>` before the first
    other #include in the file, preserving the existing include block."""
    text = path.read_text(encoding="utf-8-sig", errors="replace")
    match = ANY_INCLUDE_RE.search(text)
    line = "#include <nimrtc/core/api_version.hpp>"
    if match is None:
        # No include block at all — append after the include guard.
        guard = re.search(r"#ifndef\s+\w+\n#define\s+\w+\n", text)
        if guard is None:
            print(f"  SKIP (no include guard): {path}")
            return False
        insert_at = guard.end()
        new_text = text[:insert_at] + "\n" + line + "\n" + text[insert_at:]
    else:
        insert_at = match.start()
        new_text = text[:insert_at] + line + "\n" + text[insert_at:]

    encoding = "utf-8-sig" if path.read_bytes().startswith(b"\xef\xbb\xbf") else "utf-8"
    path.write_text(new_text, encoding=encoding)
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--path", default="src", help="subtree to scan (default: src)")
    parser.add_argument("--fix", action="store_true", help="insert the missing include")
    args = parser.parse_args()

    root = pathlib.Path(args.path)
    if not root.is_dir():
        print(f"ERROR: {root} is not a directory", file=sys.stderr)
        return 2

    offenders = find_offenders(root)
    if not offenders:
        print(f"OK: every marker-using header under {root}/ includes api_version.hpp")
        return 0

    print(f"{len(offenders)} header(s) use a stability marker without "
          f"including nimrtc/core/api_version.hpp:\n")
    for path in offenders:
        print(f"  {path}")

    if args.fix:
        print("\n--fix: inserting include...")
        failed: list[pathlib.Path] = []
        for path in offenders:
            if insert_include(path):
                print(f"  patched {path}")
            else:
                failed.append(path)
        if failed:
            print(f"\n{len(failed)} file(s) could not be patched automatically",
                  file=sys.stderr)
            return 1
        print("\nDone. Re-run without --fix to confirm.")
        return 0

    return 1


if __name__ == "__main__":
    sys.exit(main())