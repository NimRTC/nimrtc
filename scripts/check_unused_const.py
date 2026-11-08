#!/usr/bin/env python3
"""Report namespace-scope constants that nothing references.

Clang's -Wunused-const-variable fires on an internal-linkage constant
with no reader.  NimRTC compiles with -Werror, and GCC does not
diagnose the same construct, so such a constant is invisible on the
Windows and Linux builds and only breaks the Apple Clang leg.

A constant declared in a *header* is a different case: it has external
linkage when declared `inline constexpr`, and Clang does not warn about
it.  This script therefore only looks at .cpp files.

## Linkage decides the scope of the search

A namespace-scope `const` / `constexpr` in a .cpp has *internal*
linkage unless something gives it external linkage:

  * a matching `extern const <type> <name>;` declaration in a header
    (or in a sibling .cpp) makes the definition external, and Clang
    does not warn about it;
  * an `inline` variable has external linkage since C++17, and Clang
    does not warn about it either.

Only an internal-linkage constant can be diagnosed at all, and an
internal-linkage name is invisible outside its own translation unit.
So for such a constant the reference search is scoped to *its own
file* -- a mention in a sibling .cpp cannot possibly be a use.

## What counts as a candidate

A candidate is a `const` / `constexpr` declaration that

  * starts in column 1 (namespace scope -- an indented declaration is a
    class member or a function local, neither of which is diagnosed the
    same way), and
  * carries no `[[maybe_unused]]` attribute, and
  * has internal linkage (no `inline`, no `extern const` declaration
    elsewhere), and
  * is never mentioned again in its own file.

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

# A namespace-scope declaration that starts with `const` / `constexpr`.
# The declaration must start in column 1: an indented declaration is
# either a class member (external linkage rules differ) or a function
# local (diagnosed as -Wunused-variable, not -Wunused-const-variable).
DECL_START = re.compile(
    r"^(?:(?:inline|static|constexpr|const|extern|thread_local)[ \t]+)*"
    r"\b(?:constexpr|const)\b",
    re.MULTILINE,
)

# Qualifiers that may precede the type of a declaration.
_QUALIFIERS = ("inline", "static", "constexpr", "const", "extern", "thread_local")

# Type keywords that may be followed by a braced aggregate body.
_AGGREGATE_KEYWORDS = frozenset(("struct", "class", "enum", "union"))

# Words that can never name a constant.
_KEYWORDS = frozenset(
    """
    alignas alignof and asm auto bool break case catch char char8_t char16_t char32_t
    class concept const consteval constexpr constinit const_cast continue co_await
    co_return co_yield decltype default delete do double dynamic_cast else enum
    explicit export extern false float for friend goto if inline int long mutable
    namespace new noexcept nullptr operator private protected public register
    reinterpret_cast requires return short signed sizeof static static_assert
    static_cast struct switch template this thread_local throw true try typedef
    typeid typename union unsigned using virtual void volatile wchar_t while
    """.split()
)

IDENT = re.compile(r"\b[A-Za-z_]\w*\b")
WORD = re.compile(r"[A-Za-z_]\w*")

# An `extern const` declaration gives the definition external linkage.
# Only that exact shape counts -- `extern "C"` blocks and `extern`
# function declarations are unrelated.
EXTERN_DECL = re.compile(
    r"\bextern\s+const\b[^;{]*?\b(?P<name>[A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*;",
    re.MULTILINE,
)

# Line comments and block comments.  A name that is only *mentioned* in
# a comment is not a reference.
LINE_COMMENT = re.compile(r"//[^\n]*")
BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)
STRING_LITERAL = re.compile(r'"(?:\\.|[^"\\])*"')

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


def _blank(match: "re.Match[str]") -> str:
    """Replace a match with spaces, keeping its newlines.

    Line numbers must survive the stripping: the report points at a
    line in the original file.
    """
    return "".join(ch if ch == "\n" else " " for ch in match.group(0))


def strip_noise(text: str) -> str:
    """Remove comments and string literals so mentions do not count."""
    text = BLOCK_COMMENT.sub(_blank, text)
    text = LINE_COMMENT.sub(_blank, text)
    return STRING_LITERAL.sub('""', text)


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


def _last_ident(idents: list[str]) -> str | None:
    """The declarator name: the last identifier that is not a keyword."""
    for word in reversed(idents):
        if word not in _KEYWORDS:
            return word
    return None


def parse_declaration(text: str, start: int) -> tuple[str, str] | None:
    """Parse the declaration that begins at `start`.

    Returns (qualifiers, name), or None when the text is not a
    constant definition this gate should care about.
    """
    n = len(text)
    i = start

    # ---- qualifiers ----------------------------------------------------
    quals: list[str] = []
    while True:
        m = WORD.match(text, i)
        if not m:
            break
        word = m.group(0)
        if word not in _QUALIFIERS:
            break
        quals.append(word)
        i = m.end()
        while i < n and text[i] in " \t":
            i += 1

    if "const" not in quals and "constexpr" not in quals:
        return None

    # ---- type, declarator, initializer ----------------------------------
    idents: list[str] = []
    depth = 0            # nesting of ( [ {
    in_type_body = False  # inside the braces of an aggregate *type*
    aggregate_type = False
    prev_significant = ""

    while i < n:
        c = text[i]

        if c.isalpha() or c == "_":
            m = WORD.match(text, i)
            word = m.group(0)
            if not in_type_body:
                idents.append(word)
            i = m.end()
            prev_significant = "x"
            continue

        if c == "{":
            if depth == 0:
                if aggregate_type:
                    # The initializer of a variable whose type is an
                    # aggregate: `constexpr struct { int a; } kFoo{1};`
                    break
                if idents and (
                    idents[-1] in _AGGREGATE_KEYWORDS
                    or (len(idents) >= 2 and idents[-2] in _AGGREGATE_KEYWORDS)
                ):
                    # A braced aggregate *type*, not an initializer.
                    aggregate_type = True
                    in_type_body = True
                    depth = 1
                    i += 1
                    continue
                # A brace initializer of a plain type.
                break
            depth += 1
            i += 1
            continue

        if c in ")]}":
            if depth == 0:
                return None
            depth -= 1
            if depth == 0:
                in_type_body = False
            i += 1
            prev_significant = c
            continue

        if c == "(":
            if depth == 0 and prev_significant == "x":
                # A function declarator: `constexpr int f() { ... }`.
                return None
            depth += 1
            i += 1
            continue

        if c == "[":
            # An array declarator: `constexpr int kFoo[3] = ...;`.
            depth += 1
            i += 1
            continue

        if c == "=" and depth == 0:
            nxt = text[i + 1] if i + 1 < n else ""
            if nxt == "=" or prev_significant in "=!<>+-*/%&|^":
                # A comparison or an assignment operator, not an
                # initializer.
                i += 1
                prev_significant = c
                continue
            break

        if c == ";" and depth == 0:
            # A declaration without an initializer.
            return None

        if c == "," and depth == 0:
            # Several declarators: only the first one is reported.
            break

        if not c.isspace():
            prev_significant = c
        i += 1

    name = _last_ident(idents)
    if not name:
        return None
    return " ".join(quals), name


def main(argv: list[str]) -> int:
    root = Path(argv[1]) if len(argv) > 1 and argv[1] else Path.cwd()
    root = root.resolve()

    sources = [
        p for p in sorted(root.rglob("*.cpp"))
        if p.is_file() and not is_skipped(p, root)
    ]

    corpus = {p: p.read_text(encoding="utf-8", errors="replace") for p in sources}
    code = {p: strip_noise(t) for p, t in corpus.items()}

    # Names whose definition has external linkage, so Clang will not
    # diagnose them and the reference search must span the whole tree.
    external: set[str] = set()
    headers = [
        p for p in sorted(root.rglob("*"))
        if p.is_file()
        and p.suffix in {".hpp", ".h", ".hh", ".hxx"}
        and not is_skipped(p, root)
    ]
    for path in headers:
        text = strip_noise(path.read_text(encoding="utf-8", errors="replace"))
        external.update(m.group("name") for m in EXTERN_DECL.finditer(text))
    for text in code.values():
        external.update(m.group("name") for m in EXTERN_DECL.finditer(text))

    found = 0
    for path, text in code.items():
        counts: Counter[str] = Counter(IDENT.findall(text))
        for match in DECL_START.finditer(text):
            parsed = parse_declaration(text, match.start())
            if parsed is None:
                continue
            quals, name = parsed
            if has_maybe_unused(text, match.start()):
                continue
            # An `inline` variable has external linkage since C++17, and
            # an `extern` definition is external by definition.
            if "inline" in quals or "extern" in quals:
                continue
            # An `extern const` declaration elsewhere gives the
            # definition external linkage: Clang stays quiet and the
            # use may live in any translation unit.
            if name in external:
                continue
            # Internal linkage: only this file can possibly read it.
            if counts[name] <= 1:
                line = text[: match.start()].count("\n") + 1
                print("%s:%d: %s" % (path, line, name))
                found += 1
    print("%d candidate(s)" % found, file=sys.stderr)
    return 1 if found else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
