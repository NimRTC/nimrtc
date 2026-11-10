#!/usr/bin/env python3
"""Repair the GBK mojibake in the C++ sources.

A tool opened the UTF-8 sources with a Chinese-codepage decoder (GBK)
and saved them back.  The damage has two shapes.

A three-byte UTF-8 sequence whose first two bytes form a valid GBK
pair became that single GBK character.  The third byte was then paired
with the byte that followed it; that pair was invalid, so the decoder
replaced both bytes with one question mark.

    "—"  (e2 80 94)  ->  U+9225 + "?"
    "→"  (e2 86 92)  ->  U+922B + "?"

The byte that followed the character was a space, and it was consumed
by the invalid pair, so the space has to come back.  The question mark
is the marker of the damage and is removed with it.

    "— the"  ->  U+9225 + "?" + "the"  ->  "— the"

A two-byte UTF-8 sequence that is a valid GBK pair all by itself lost
nothing but the character.

    "§"  (c2 a7)  ->  U+6402
    "×"  (c3 97)  ->  U+8133

Some files were written back in GBK rather than in UTF-8, so there the
damage sits at the byte level: the GBK encoding of the mangled
character followed by the question mark.

    "—"  ->  e2 80 3f  ->  e2 80 94 20

The rules are written with explicit escapes so that this script cannot
be damaged by the same mistake it repairs.

Usage:  python tools/fix_gbk_mojibake.py [--check] [paths...]
"""
import argparse
import pathlib
import sys

# Mangled text -> the text it was before the mangling.
TEXT_FIXES = [
    ("\u9225?", "\u2014 "),   # em dash and the space it ate
    ("\u922b?", "\u2192 "),   # rightwards arrow and the space it ate
    ("\u6402", "\u00a7"),     # section sign
    ("\u8133", "\u00d7"),     # multiplication sign
]

# Damaged bytes -> the bytes they were before the mangling, for the
# files that were written back in GBK.  Neither sequence can occur in
# valid UTF-8, so these rules cannot touch healthy text.
BYTE_FIXES = [
    (b"\xe2\x80\x3f", b"\xe2\x80\x94 "),   # em dash, space eaten
    (b"\xe2\x86\x3f", b"\xe2\x86\x92 "),   # rightwards arrow, space eaten
]

# Directories that are never first-party sources.
SKIP_DIRS = {"third_party", "build", ".git", "node_modules", "__pycache__"}


def repair_bytes(raw: bytes) -> bytes:
    for broken, whole in BYTE_FIXES:
        raw = raw.replace(broken, whole)
    text = raw.decode("utf-8", errors="surrogateescape")
    for broken, whole in TEXT_FIXES:
        text = text.replace(broken, whole)
    return text.encode("utf-8", errors="surrogateescape")


def damaged_files(root: pathlib.Path) -> list:
    """Every first-party file that still carries the damage."""
    found = []
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        if set(path.relative_to(root).parts) & SKIP_DIRS:
            continue
        raw = path.read_bytes()
        if any(broken in raw for broken, _ in BYTE_FIXES):
            found.append(path)
            continue
        text = raw.decode("utf-8", errors="replace")
        if any(broken in text for broken, _ in TEXT_FIXES):
            found.append(path)
    return found


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="*", default=None,
                    help="files to repair; defaults to the whole tree")
    ap.add_argument("--check", action="store_true",
                    help="report the damage without writing it back")
    args = ap.parse_args()

    root = pathlib.Path(__file__).resolve().parent.parent
    paths = ([pathlib.Path(p) for p in args.paths]
             if args.paths else damaged_files(root))
    if not paths:
        print("no damage found")
        return 0

    broken_files = 0
    for path in paths:
        raw = path.read_bytes()
        fixed = repair_bytes(raw)
        if fixed == raw:
            continue
        broken_files += 1
        if args.check:
            print(f"{path}: damaged")
        else:
            path.write_bytes(fixed)
            print(f"{path}: repaired")
    if args.check and broken_files:
        print(f"{broken_files} file(s) still carry the mojibake")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
