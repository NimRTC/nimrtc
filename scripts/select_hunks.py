#!/usr/bin/env python3
"""Split a unified diff into the hunks we want to stage.

Usage:
    python scripts/select_hunks.py <file> <hunk-index>...        # to stdout
    python scripts/select_hunks.py -o <out.patch> <file> <idx>...  # to a file

The hunk index is 1-based and counted in the order the hunks appear in
the diff.  Every selected hunk is written out verbatim; the rest are
dropped.  The result is a patch that applies to the index with
`git apply --cached`.

Writing to a file (rather than stdout) is the reliable path on Windows:
a PowerShell stdout redirect re-encodes the patch as UTF-16, which
`git apply` cannot read.
"""
import argparse
import subprocess
import sys


def collect(path: str, wanted: set):
    diff = subprocess.run(
        ["git", "--no-pager", "diff", "--", path],
        capture_output=True, check=True).stdout.decode("utf-8", errors="replace")

    lines = diff.splitlines(keepends=True)

    header_end = 0
    for i, line in enumerate(lines):
        if line.startswith("@@"):
            header_end = i
            break
    header = lines[:header_end]

    hunks = []
    cur = None
    for line in lines[header_end:]:
        if line.startswith("@@"):
            if cur is not None:
                hunks.append(cur)
            cur = [line]
        elif cur is not None:
            cur.append(line)
    if cur is not None:
        hunks.append(cur)

    out = list(header)
    for idx, hunk in enumerate(hunks, start=1):
        if idx in wanted:
            out.extend(hunk)
    return "".join(out), len(hunks)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("-o", "--output", help="write the patch to this file")
    ap.add_argument("path")
    ap.add_argument("hunks", nargs="+", type=int)
    args = ap.parse_args()

    wanted = set(args.hunks)
    text, total = collect(args.path, wanted)
    if total == 0:
        print(f"error: {args.path} has no hunks", file=sys.stderr)
        return 2
    if max(wanted) > total:
        print(f"error: {args.path} has {total} hunks, asked for {sorted(wanted)}",
              file=sys.stderr)
        return 2

    if args.output:
        with open(args.output, "w", encoding="utf-8", newline="") as fh:
            fh.write(text)
        print(f"selected hunks {sorted(wanted)} of {total} -> {args.output}",
              file=sys.stderr)
    else:
        sys.stdout.buffer.write(text.encode("utf-8"))
        print(f"selected hunks {sorted(wanted)} of {total}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
