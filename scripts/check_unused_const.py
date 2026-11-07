#!/usr/bin/env python3
"""Report namespace-scope constants that nothing references.

Clang's -Wunused-const-variable fires on an internal-linkage constant
with no reader.  NimRTC compiles with -Werror, and GCC does not
diagnose the same construct, so such a constant is invisible on the
Windows and Linux builds and only breaks the Apple Clang leg.

A constant declared in a *header* is a different case: it has external
linkage when declared `inline constexpr`, and Clang does not warn about
it.  This script therefore only looks at .cpp files.  The reference
count is computed over every .cpp in the tree in a single pass, so a
constant used from a sibling translation unit is not mistaken for an
unused one.

## What counts as a candidate

A candidate is a `const` / `constexpr` declaration that

  * starts in column 1 (namespace scope -- an indented declaration is a
    class member or a function local, neither of which is diagnosed the
    same way), and
  * carries no `[[maybe_unused]]` attribute, and
  * is never mentioned again in any .cpp file of the tree.

## `[[maybe_unused]]` and the preprocessor guard

Several declarations guard the attribute behind a compiler check:

    #if defined(__clang__)
    [[maybe_unused]]
    #endif
    constexpr std::size_t kSamplesPer10Ms48K = 480;

The attribute only reaches the compiler on Clang, and Clang is the
compiler that warns -- so such a declaration is fine.  The attribute
scan therefore walks backwards over blank lines and preprocessor lines
and stops at the first real line.

## Usage

    python scripts/check_unused_const.py [root]

Exits 1 when at least one candidate is found, so it can be used as a CI
gate.  On a clean tree it prints "0 candidate(s)" and exits 0.
"""

from __future__ import annotations

import re
import sys
from collections import Counter
from pathlib import Path

# A namespace-scope `constexpr`/`const` declaration.  `inline` and
# `static` are allowed; `inline constexpr` in a .cpp is still internal
# to the translation unit for the purpose of this check.
#
# The declaration must start in column 1: an indented `constexpr` is
# either a class member (external linkage rules differ) or a function
# local (diagnosed as -Wunused-variable, not -Wunused-const-variable).
DECL = re.compile(
    r"^(?P<qual>(?:inline[ \t]+|static[ \t]+|constexpr[ \t]+|const[ \t]+)*)"
    r"(?P<kw>constexpr|const)\b"
    r"(?P<type>[^;=()]*?)"
    r"\b(?P<name>[A-Za-z_]\w*)\s*=\s*[^=]",
    re.MULTILINE,
)

IDENT = re.compile(r"\b[A-Za-z_]\w*\b")

# Vendored trees and build output are not NimRTC's code.
SKIP_PARTS = ("third_party", "googletest", "wolfssl", "build")

# Lines that may sit between the `[[maybe_unused]]` attribute and the
# declaration it annotates.
_ATTR_SCAN_LINES = 6


def is_skipped(path: Path, root: Path) -> bool:
    try:
        parts = path.relative_to(root).parts
    except ValueError:
        return False
    return any(part in SKIP_PARTS or part.startswith("build") for part in parts)


def has_maybe_unused(text: str, decl_start: int) -> bool:
    """True when `[[maybe_unused]]` annotates the declaration at `decl_start`."""
    head = text[:decl_start].rstrip("\n")
    lines = head.split("\n")
    # Walk backwards: blank lines and preprocessor lines do not break the
    # association between the attribute and the declaration.
    for line in reversed(lines[-_ATTR_SCAN_LINES:]):
        stripped = line.strip()
        if not stripped:
            continue
        if stripped.startswith("#"):
            continue
        return "[[maybe_unused]]" in stripped
    return False


def main(argv: list[str]) -> int:
    root = Path(argv[1]) if len(argv) > 1 else Path.cwd()
    sources = [
        p for p in sorted(root.rglob("*.cpp"))
        if p.is_file() and not is_skipped(p, root)
    ]

    corpus = {p: p.read_text(encoding="utf-8", errors="replace") for p in sources}
    counts: Counter[str] = Counter()
    for text in corpus.values():
        counts.update(IDENT.findall(text))

    found = 0
    for path, text in corpus.items():
        for match in DECL.finditer(text):
            name = match.group("name")
            if not name or name in {"constexpr", "const"}:
                continue
            if has_maybe_unused(text, match.start()):
                continue
            # A constant referenced anywhere in the tree is not unused;
            # the reference may legitimately live in a sibling .cpp.
            if counts[name] <= 1:
                line = text[: match.start()].count("\n") + 1
                print("%s:%d: %s" % (path, line, name))
                found += 1
    print("%d candidate(s)" % found, file=sys.stderr)
    return 1 if found else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
