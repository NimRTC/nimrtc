#!/usr/bin/env python3
"""Census of every non-ASCII character in the first-party tree.

Lists the distinct codepoints per file together with their GBK
encoding, so the codepage damage can be told apart from the text that
was always there.  Read-only: it never writes.
"""
import collections
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SKIP = {"third_party", "build", ".git", "node_modules", "__pycache__"}


def main() -> int:
    per_file = collections.defaultdict(collections.Counter)
    for path in sorted(ROOT.rglob("*")):
        if not path.is_file():
            continue
        rel = path.relative_to(ROOT)
        if set(rel.parts) & SKIP:
            continue
        if path.suffix not in {".cpp", ".h", ".hpp", ".c", ".cc", ".md",
                               ".txt", ".py", ".json", ".yml", ".yaml"}:
            continue
        raw = path.read_bytes()
        if any(b > 127 for b in raw):
            text = raw.decode("utf-8", errors="replace")
            for ch in text:
                if ord(ch) > 127:
                    per_file[str(rel)][ch] += 1

    for name, counter in per_file.items():
        print(name)
        for ch, n in sorted(counter.items()):
            gbk = ch.encode("gbk", errors="replace").hex(" ")
            print(f"    U+{ord(ch):04X} x{n:<4} gbk={gbk}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
