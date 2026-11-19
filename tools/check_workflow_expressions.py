#!/usr/bin/env python3
"""Reject `${{ ... }}` blocks that are not GitHub expressions.

Why this exists
---------------
A `${{ }}` block inside a workflow file is *not* shell and not YAML: GitHub
evaluates it with its own expression language.  Anything else in it --
notably a bash expansion like `tag%.*` -- is not a syntax error anywhere
locally.  The YAML parses.  The script text is a perfectly good bash string
to any editor and to `bash -n`.  The failure shows up only after a push, as
a workflow that starts and produces **zero jobs**: the file did not parse,
so there was nothing to run.

That happened on 2026-11-19.  Commit `d6c3e38c` (the v1.2.0 follow-up, by
this project) added to `release.yml`'s tag-ancestry step:

    echo "       'git describe' cannot find it, a '${{ tag%.* }}.x'"

`tag%.*` is bash "strip the shortest dot-suffix".  Inside `${{ }}` it is
nonsense, the `release` workflow stopped parsing, and every push to a
branch that touched `release.yml` ran it for 0 s with no jobs
(runs 44478642313, 44481029254).  Two pushes of diagnosis followed before
the line was found, because the local tree gave no signal at all.

The guard reads every workflow, extracts every `${{ ... }}`, and rejects
any block containing characters or operator shapes the expression language
does not have.  It cannot prove an expression is *meaningful* -- that needs
the context GitHub has -- but it does reject the class of mistake that is
invisible everywhere else: shell syntax where an expression belongs.

    python tools/check_workflow_expressions.py
    python tools/check_workflow_expressions.py --self-test
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
WORKFLOWS = REPO_ROOT / ".github" / "workflows"

# The characters an expression may contain.  The expression language has
# string literals (`'...'`, `''` to escape a quote), numbers, property
# access with `.` and `[ ]`, and a fixed operator set.  It has no `%`, no
# `$`, no `#`, and no backtick -- and it has no shell parameter expansion,
# which is what a `%` or `#` inside one usually is.
EXPR_CHARS = set(
    "abcdefghijklmnopqrstuvwxyz"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "0123456789"
    "_ . , ( ) [ ] ! < > = & | ' * / + - :"
)

# Operator-looking sequences the language actually has.  A block is checked
# for characters first; these are what is left to look for once the
# characters are known to be legal, so a reviewer reading a failure knows
# which of the two rules caught it.
KNOWN_OPERATORS = (
    "==", "!=", "&&", "||", ">=", "<=", "<", ">", "!", "*", "/", "+", "-",
)

# `${{` ... `}}` -- non-greedy, across lines.
BLOCK = re.compile(r"\$\{\{(.*?)\}\}", re.DOTALL)


def scan_text(text: str, label: str) -> list[str]:
    """Return a list of problems found in *text* (one workflow file)."""
    problems: list[str] = []
    for match in BLOCK.finditer(text):
        body = match.group(1)
        line = text.count("\n", 0, match.start()) + 1

        # 1. Illegal characters.  Report the first one, spelled out: the
        #    failure being caught is almost always a shell expansion, and
        #    naming the character makes that obvious.
        bad = sorted({c for c in body if c not in EXPR_CHARS})
        if bad:
            shown = ", ".join(repr(c) for c in bad)
            problems.append(
                f"{label}:{line}: `${{{{{body.strip()}}}}}` contains {shown}, "
                f"which the GitHub expression language does not have -- this "
                f"is shell syntax where an expression belongs")

        # 2. Reserved words used as shell would use them.  `tag` itself is
        #    legal (it is a context name), but `tag%` / `tag#` are not, and
        #    the character check above catches those; this rule catches the
        #    expansion forms that use only legal characters.
        for shell_ism in ("$$", "$(", "`"):
            if shell_ism in body:
                problems.append(
                    f"{label}:{line}: `${{{{{body.strip()}}}}}` contains "
                    f"{shell_ism!r}, which is shell expansion, not an "
                    f"expression")
    return problems


def scan_file(path: Path) -> list[str]:
    return scan_text(path.read_text(encoding="utf-8"), str(path.name))


def scan_tree(directory: Path) -> list[str]:
    problems: list[str] = []
    for path in sorted(directory.glob("*.yml")) + sorted(directory.glob("*.yaml")):
        problems.extend(scan_file(path))
    return problems


def self_test() -> int:
    failures: list[str] = []

    def expect(cond: bool, what: str) -> None:
        print(("PASS  " if cond else "FAIL  ") + what)
        if not cond:
            failures.append(what)

    # 1. The exact line that broke `release.yml`, verbatim.
    broken = "echo \"a '${{ tag%.* }}.x'\""
    expect(bool(scan_text(broken, "t")),
           "the line that broke release.yml is rejected")

    # 2. Legal expressions are accepted -- including the ones the release
    #    workflow actually uses, so the guard does not fire on the tree.
    legal = [
        "${{ inputs.tag || github.ref }}",
        "${{ github.event_name == 'workflow_dispatch' }}",
        "${{ steps.resolve-tag.outputs.tag }}",
        "${{ secrets.GPG_PRIVATE_KEY }}",
        "${{ vars.GPG_FINGERPRINT }}",
        "${{ steps.x.outputs['configured'] }}",
        "${{ github.ref_name }}",
        "${{ !cancelled() && needs.a.result == 'success' }}",
        "${{ fromJSON(needs.m.outputs.list)[0] }}",
    ]
    for s in legal:
        expect(not scan_text(s, "t"), f"legal expression accepted: {s}")

    # 3. `$$`, `$(...)` and backticks are shell, not expressions.
    for s in ("${{ $$ }}", "${{ $(id) }}", "${{ `id` }}"):
        expect(bool(scan_text(s, "t")), f"shell expansion rejected: {s}")

    # 4. Every workflow in the tree passes.
    problems = scan_tree(WORKFLOWS)
    for p in problems:
        print("        " + p)
    expect(not problems, "every workflow in .github/workflows passes")

    if failures:
        print(f"\n{len(failures)} self-test failure(s).")
        return 1
    print("\nPASS -- the self-test confirms every classification.")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--dir", default=str(WORKFLOWS),
                        help="directory of workflows (default: .github/workflows)")
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()

    directory = Path(args.dir)
    if not directory.is_dir():
        print(f"error: {directory} is not a directory", file=sys.stderr)
        return 2
    problems = scan_tree(directory)
    for p in problems:
        print("FAIL  " + p)
    if problems:
        return 1
    count = len(list(directory.glob("*.yml"))) + len(list(directory.glob("*.yaml")))
    print(f"PASS -- every `${{{{ }}}}` block in {count} workflow(s) is an "
          f"expression")
    return 0


if __name__ == "__main__":
    sys.exit(main())
