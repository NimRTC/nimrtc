#!/usr/bin/env python3
"""Scan public headers for the "nested struct defined in the return type"
pattern, which GCC and Clang both reject outright:

    struct Foo { ... } name() const noexcept;   // error: new types may not
                                                // be defined in a return type

The struct must be declared first and then named as the return type:

    struct Foo { ... };
    Foo name() const noexcept;
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"

# A closing brace of a struct/class body, followed by an identifier, then '('.
PATTERN = re.compile(r"^\s*\}\s*([A-Za-z_][A-Za-z0-9_]*)\s*\(", re.MULTILINE)

# Names that can never be a return type: control-flow keywords, the
# log macros, and the macro invocation helpers.
NEVER_A_TYPE = {
    "if", "while", "for", "switch", "catch", "return", "do", "else",
    "NIMRTC_LOG_INFO", "NIMRTC_LOG_WARN", "NIMRTC_LOG_ERROR",
    "NIMRTC_LOG_DEBUG", "NIMRTC_LOG_TRACE",
}

SKIP = {"third_party", "build"}


def enclosing_class_names(text: str) -> set:
    """The names of the classes/structs whose bodies enclose a given
    offset.  A constructor's name always matches one of them."""
    names = set()
    # A rough scan: every `class X` / `struct X` opening in the file.
    for m in re.finditer(r"^\s*(?:class|struct)\s+(?:[A-Z_][A-Z0-9_]*\s+)?([A-Za-z_][A-Za-z0-9_]*)",
                         text, re.MULTILINE):
        names.add(m.group(1))
    return names


def main() -> int:
    hits = []
    for path in sorted(SRC.rglob("*.hpp")):
        rel = path.relative_to(ROOT)
        if any(part in SKIP for part in rel.parts):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        declared = enclosing_class_names(text)
        for m in PATTERN.finditer(text):
            name = m.group(1)
            if name in NEVER_A_TYPE:
                continue
            if name in declared:
                # A constructor of that class: legal.
                continue
            line_no = text.count("\n", 0, m.start()) + 1
            hits.append((str(rel), line_no, name))

    if not hits:
        print("OK: no 'new type in return type' declarations found")
        return 0

    print(f"FOUND {len(hits)} suspect declaration(s):")
    for rel, line_no, name in hits:
        print(f"  {rel}:{line_no}: {name}()")
    return 1


if __name__ == "__main__":
    sys.exit(main())
