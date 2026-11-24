#!/usr/bin/env python3
"""check_sample_line_budget.py -- re-measure the SDK sample's line budget.

## Why this exists

The v1-sdk-sample slice claims the sample fits in 200 non-comment lines,
and the claim is a fact to be re-measured, not an assertion to be
trusted: a limit that drifts is a claim that drifted (plan §5 N6).  This
script is the measurement.  CI runs it on every push, so a change that
pushes the sample past the limit fails the gate *before* merge.

## What it counts

Every `.cpp` / `.hpp` under `examples/sdk-sample/` (recursively).  A
"non-comment line" is one that carries any code:

  * empty lines do not count;
  * a line whose first token is `//` does not count;
  * every line of a `/* ... */` block comment does not count -- including
    the first and last line;
  * a block comment followed by code on the same line DOES count
    (`/* x */ int y = 1;` is one line);
  * `//` inside a string literal is NOT a comment (`"http://host"` is
    code, and the second half of the line is not discarded).

The naive `split('//')` approach is rejected on purpose: a signalling
URL in the sample would be miscounted as a comment, the line count
would be understated, and the budget would leak.

## Judgement

  * total <= budget (200)                          -> clean pass
  * budget < total <= budget * (1 + drift) (220)   -> pass with a warning
  * total > budget * (1 + drift)                   -> fail (non-zero exit)

The drift allowance is 10%, declared next to the budget in
`examples/sdk-sample/sample_line_budget.json`.  The exemption list in
that file must stay empty (or contain only `README.md`, which is not
counted anyway): a non-empty list would let code hide from the count.

## Usage

    python tools/check_sample_line_budget.py [--repo-root ROOT]
    python tools/check_sample_line_budget.py --self-test

Exit code 0 when the sample is within the limit, 1 otherwise.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# The counter
# ---------------------------------------------------------------------------


def count_noncomment_lines(text: str) -> int:
    """Count non-comment code lines in C/C++ source.

    A line counts when it carries any code outside comments and string
    literals.  `/* ... */` may span lines; `//` runs to end of line;
    `//` inside a string literal is data, not a comment.
    """
    in_block = False
    count = 0
    for line in text.splitlines():
        has_code, in_block = _scan_line(line, in_block)
        if has_code:
            count += 1
    return count


def _scan_line(line: str, in_block: bool) -> tuple[bool, bool]:
    n = len(line)
    i = 0
    has_code = False
    while i < n:
        c = line[i]
        if in_block:
            if c == "*" and i + 1 < n and line[i + 1] == "/":
                in_block = False
                i += 2
            else:
                i += 1
        elif c == '"' or c == "'":
            # A string or char literal: a comment marker inside it is
            # data.  An opening quote is itself code, so the line counts.
            has_code = True
            quote = c
            i += 1
            while i < n:
                if line[i] == "\\":
                    i += 2
                elif line[i] == quote:
                    i += 1
                    break
                else:
                    i += 1
        elif c == "/" and i + 1 < n and line[i + 1] == "/":
            # `//` line comment: the rest of the line is not code.
            i = n
        elif c == "/" and i + 1 < n and line[i + 1] == "*":
            in_block = True
            i += 2
        else:
            if not c.isspace():
                has_code = True
            i += 1
    return has_code, in_block


# ---------------------------------------------------------------------------
# The check
# ---------------------------------------------------------------------------

DEFAULT_BUDGET = 200
DEFAULT_DRIFT = 0.10
# Only README.md may appear in the exemption list, and it is not a
# `.cpp` / `.hpp`, so an empty list is the only meaningful one.
ALLOWED_EXEMPTIONS = {"README.md"}


def load_declaration(repo_root: Path) -> dict:
    path = repo_root / "examples" / "sdk-sample" / "sample_line_budget.json"
    if not path.is_file():
        raise SystemExit("error: %s not found" % path)
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise SystemExit("error: %s is not valid JSON: %s" % (path, exc))
    budget = int(data.get("budget", DEFAULT_BUDGET))
    drift = float(data.get("drift_allowance", DEFAULT_DRIFT))
    exemptions = list(data.get("exemptions", []))
    return {"budget": budget, "drift_allowance": drift,
            "exemptions": exemptions}


def scan_sample(sample_dir: Path) -> list[tuple[str, int]]:
    files = sorted(p for p in sample_dir.rglob("*")
                   if p.is_file() and p.suffix in (".cpp", ".hpp"))
    return [(str(p.relative_to(sample_dir)),
             count_noncomment_lines(p.read_text(encoding="utf-8")))
            for p in files]


def check(repo_root: Path, verbose: bool = False) -> list[str]:
    """Return a list of problem lines (empty = pass)."""
    problems: list[str] = []

    decl = load_declaration(repo_root)
    budget = decl["budget"]
    limit = int(budget * (1.0 + decl["drift_allowance"]))

    bad = [e for e in decl["exemptions"] if e not in ALLOWED_EXEMPTIONS]
    if bad:
        problems.append(
            "the exemption list must be empty (or only README.md); it "
            "contains: %s" % ", ".join(bad))

    sample_dir = repo_root / "examples" / "sdk-sample"
    if not sample_dir.is_dir():
        problems.append("examples/sdk-sample/ does not exist")
        return problems

    counts = scan_sample(sample_dir)
    if not counts:
        problems.append("examples/sdk-sample/ has no .cpp / .hpp to count")
        return problems

    total = sum(n for _, n in counts)
    if verbose:
        for name, n in counts:
            print("  %-28s %4d" % (name, n))
        print("  %-28s %4d  (budget %d, limit %d)"
              % ("total", total, budget, limit))

    if total > limit:
        details = ", ".join("%s: %d" % (name, n) for name, n in counts)
        problems.append(
            "examples/sdk-sample/ has %d non-comment lines, which exceeds "
            "the limit %d (budget %d + %d%% drift): %s"
            % (total, limit, budget, int(decl["drift_allowance"] * 100),
               details))
    elif total > budget:
        print("WARN -- examples/sdk-sample/ is at %d non-comment lines, "
              "above the %d budget but within the %d%% drift limit; tighten "
              "it before the next slice" % (total, budget,
                                            int(decl["drift_allowance"] * 100)))
    return problems


# ---------------------------------------------------------------------------
# The self-test
# ---------------------------------------------------------------------------

SELF_TEST_CASES = [
    # (label, source text, expected line count)
    ("http-url-line",
     'const char* url = "http://host:5349?transport=tcp";\n', 1),
    ("multiline-block-comment",
     "/* first\nsecond\nthird */\n", 0),
    ("block-comment-then-code",
     "/* why */ int y = 1;\n", 1),
    ("line-comment-lines",
     "// only a comment\n   // indented comment\n", 0),
    ("string-with-slash-slash",
     'const char* s = "a//b";\n', 1),
    ("empty-lines",
     "\n\n   \n", 0),
    ("block-then-line",
     "/* a */\n// b\nint x = 2;\n", 1),
    ("trailing-line-comment",
     "int x = 2; // note\n", 1),
]


def self_test() -> int:
    failures: list[str] = []

    def expect(condition: bool, message: str) -> None:
        if not condition:
            failures.append(message)

    for label, text, expected in SELF_TEST_CASES:
        got = count_noncomment_lines(text)
        expect(got == expected,
               "%s: count_noncomment_lines(...) = %d, expected %d"
               % (label, got, expected))

    # The multi-line block-comment continuation rule: the lines between
    # `/*` and `*/` do not count, and a line that closes the block and
    # then carries code counts once.
    got = count_noncomment_lines("int a = 1;\n/* open\n"
                                 "middle\nclose */ int b = 2;\n")
    expect(got == 2, "block-span continuation: got %d, expected 2" % got)

    if failures:
        print("FAIL -- the line counter misclassified a case:")
        for f in failures:
            print("  -", f)
        return 1
    print("PASS -- the line counter matches every pinned case.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", default=None,
                        help="the repository root (default: inferred)")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    root = (Path(args.repo_root) if args.repo_root
            else Path(__file__).resolve().parent.parent)
    problems = check(root, args.verbose)

    if problems:
        print("FAIL -- the SDK sample's line budget:")
        for p in problems:
            print("  -", p)
        return 1

    print("PASS -- the SDK sample is within its line budget.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
