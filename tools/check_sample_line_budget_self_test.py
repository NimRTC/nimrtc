#!/usr/bin/env python3
"""check_sample_line_budget_self_test.py -- pin the line-budget counter.

The line-budget gate only knows the comment shapes it is told to know.
This script pins that behaviour, so the gate cannot silently drift into
misclassification.  It is the `--self-test` counterpart of
`tools/check_sample_line_budget.py`, as a standalone CI step:

    python tools/check_sample_line_budget_self_test.py

The pinned cases (slice v1-sdk-sample, spec §三):

  * a line that carries `http://` inside a string counts as **1** line
    (the naive `split('//')` would miscount it as a comment);
  * a multi-line `/* ... */` block comment counts as **0** lines
    (including its first and last line);
  * a block comment followed by code on the same line counts as **1**;
  * an over-budget sample is **reported** (the check fails);
  * a clean sample is **clean** (the check passes).

Exit code 0 when every case is pinned, 1 otherwise.
"""

from __future__ import annotations

import argparse
import json
import sys
import tempfile
from pathlib import Path

from check_sample_line_budget import check, count_noncomment_lines

BUDGET_JSON = {
    "budget": 200,
    "drift_allowance": 0.10,
    "exemptions": [],
}


def _write_sample_tree(root: Path, cpp_lines: int) -> Path:
    sample_dir = root / "examples" / "sdk-sample"
    sample_dir.mkdir(parents=True, exist_ok=True)
    (sample_dir / "sample_line_budget.json").write_text(
        json.dumps(BUDGET_JSON, indent=2), encoding="utf-8")
    lines = ["// generated sample for the self-test\n"]
    lines.append("int main() {\n")
    for i in range(cpp_lines):
        lines.append("    int unused_%d = %d;\n" % (i, i))
    lines.append("    return 0;\n}\n")
    (sample_dir / "generated.cpp").write_text(
        "".join(lines), encoding="utf-8")
    return root


def self_test(tmp_root: Path) -> int:
    failures: list[str] = []

    def expect(condition: bool, message: str) -> None:
        if not condition:
            failures.append(message)

    # ---- the counter, pinned -----------------------------------------

    expect(count_noncomment_lines('const char* url = "http://host";\n') == 1,
           "a string literal carrying `http://` must count as 1 line")
    expect(count_noncomment_lines("/* a\nb\nc */\n") == 0,
           "a multi-line block comment must count as 0 lines")
    expect(count_noncomment_lines("/* x */ int y = 1;\n") == 1,
           "a block comment followed by code on the same line must count "
           "as 1 line")
    expect(count_noncomment_lines("// only a comment\n"
                                  "int x = 2; // trailing\n") == 1,
           "a `//` comment line must not count; a code line with a "
           "trailing comment must count once")

    # ---- the gate, pinned --------------------------------------------

    with tempfile.TemporaryDirectory(dir=str(tmp_root)) as tmp:
        root = _write_sample_tree(Path(tmp), cpp_lines=300)
        problems = check(root)
        expect(bool(problems),
               "a 300-line sample must fail the gate (over the 220 limit)")
        expect(any("300" in p or "generated.cpp" in p for p in problems),
               "the over-budget report must name the file and the line count")

    with tempfile.TemporaryDirectory(dir=str(tmp_root)) as tmp:
        root = _write_sample_tree(Path(tmp), cpp_lines=4)
        problems = check(root)
        expect(not problems,
               "a 4-line sample must pass the gate cleanly")

    if failures:
        print("FAIL -- the line-budget self-test caught a misclassification:")
        for f in failures:
            print("  -", f)
        return 1
    print("PASS -- the line-budget counter and gate are pinned.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tmp-root", default=None,
                        help="scratch root for self-test trees (default: "
                             "the OS temp directory)")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        if args.tmp_root:
            tmp = args.tmp_root
        return self_test(Path(tmp))


if __name__ == "__main__":
    sys.exit(main())
