#!/usr/bin/env python3
"""Check the `release` workflow's tag filter against the tags we actually cut.

Why this exists
---------------
`release.yml` fires on `push.tags`.  GitHub matches those patterns as
ECMAScript regexes with an implicit whole-string anchor, in which `*` and
`+` quantify *the preceding character*, not the class before them.  A
pattern written in the regex idiom -- `v[0-9]+.[0-9]+.[0-9]+*` -- therefore
means "v, then one-or-more digits, then anything, ..." and matches refs that
are not tags at all.  On 2026-11-19 a push of the branch
`chore/release-1.2-followup` fired the whole release workflow; its preflight
had no tag to work with and failed in 0 seconds (run 44478642313).

Review cannot see this.  The pattern looks like the regex a reader expects,
and the mistake only surfaces as a red run *after* a push.  So it is
checked here: the workflow file is parsed, every pattern is evaluated with
ECMAScript semantics against every tag form this project cuts and against a
set of branch names, and the check fails if a tag is missed or a branch
name matches.

    python tools/check_release_tag_filter.py
    python tools/check_release_tag_filter.py --self-test

Node is the engine (see `pattern_matches`); `node` is present on every
GitHub runner and is already required by the release preflight.

Exit status: 0 when the filter is right, 1 when it is not, 2 on a usage
error (missing workflow, no patterns).
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "release.yml"

# Tags that must fire the workflow, taken from the repository's real tags
# (`gh api repos/NimRTC/nimrtc/git/refs`, 2026-11-19): every release, every
# pre-release and every interim dev tag in the history.  `v1`-style major
# aliases and `archive/*` tags are deliberately absent -- they are not
# releases and the workflow must not fire on them.
TAGS = [
    "v0.9.0-rc1",
    "v0.9.2",
    "v0.10.0",
    "v0.10.3",
    "v0.12.0",
    "v0.12.0-post1",
    "v0.14.0-beta.1",
    "v1.0.0",
    "v1.2.0",
    "v1.3.0-rc.10",
    "v1.2.1-dev.20260101",
]

# Refs that must NOT fire it.  The first is the branch that did fire it on
# 2026-11-19; the rest are the repository's real branch names at that time
# (23 of them), plus the tag-shaped non-releases.
NON_TAGS = [
    # the observed failure
    "chore/release-1.2-followup",
    # real branches
    "docs/acceptance-mutation-test-review-v2",
    "docs/architecture-remediation-plan-v2",
    "docs/v13-scope-r3-o6",
    "feat/e5-rebased",
    "feat/q1-parameter-set-switch",
    "feat/q3-rtx-rebased",
    "feat/q6-video-render-clock",
    "feat/remote-desktop-m1-m4",
    "feat/s7-qoe-collector-switch",
    "feature/capture-source-adapter",
    "feature/m1c-platform-capture",
    "feature/remediation-phase3-clean",
    "fix/e2e-ephemeral-ports",
    "main",
    "release/1.0",
    "release/1.1",
    "release/1.2",
    "version/v1",
    "version/v2",
    "version/v3",
    "version/v4",
    "version/v5",
    # tag-shaped refs that are not releases
    "v1",
    "v2",
    "archive/codec-safety",
]


def pattern_matches(pattern: str, ref: str) -> bool:
    """Evaluate one filter pattern against one ref, as GitHub does.

    Node's `RegExp`, not Python's `re`: the two disagree on exactly the
    patterns that matter here.  Python rejects `[0-9]+*` outright
    ("multiple repeat"), so a Python check could not evaluate the very
    pattern shape this check exists to catch.  ECMAScript accepts it, and
    the resulting match is the one that matters.
    """
    script = (
        "const r = new RegExp(process.argv[1]);"
        "process.stdout.write(r.test(process.argv[2]) ? '1' : '0');"
    )
    try:
        out = subprocess.run(
            ["node", "-e", script, pattern, ref],
            capture_output=True, text=True, timeout=30,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired) as exc:
        raise RuntimeError(
            f"node is required to evaluate GitHub's filter semantics: {exc}"
        ) from exc
    if out.returncode != 0:
        # A pattern ECMAScript refuses to compile never fires, which is what
        # GitHub does with a malformed filter.
        return False
    return out.stdout.strip() == "1"


def read_patterns(text: str) -> list[str]:
    """Extract `on.push.tags[*]` from the workflow *text*.

    Deliberately a line scan rather than a YAML load: the patterns contain
    characters (`[`, `]`, `*`) that different YAML loaders treat
    differently, and the file is the source of truth here.  The scan also
    yields the patterns verbatim, which is what the error message needs.
    """
    patterns: list[str] = []
    in_on = False
    in_push = False
    in_tags = False
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].rstrip()
        if not line.strip():
            continue
        indent = len(line) - len(line.lstrip())
        body = line.strip()
        if indent == 0:
            in_on = body == "on:"
            in_push = in_tags = False
            continue
        if not in_on:
            continue
        if indent == 2:
            in_push = body == "push:"
            in_tags = False
            continue
        if not in_push:
            continue
        if indent == 4:
            in_tags = body == "tags:"
            continue
        if in_tags and body.startswith("- "):
            value = body[2:].strip()
            if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
                value = value[1:-1]
            patterns.append(value)
    return patterns


def check(patterns: list[str]) -> list[str]:
    """Return a list of problems; empty means the filter is right."""
    problems: list[str] = []
    if not patterns:
        problems.append("no on.push.tags patterns found in release.yml")
        return problems

    for tag in TAGS:
        if not any(pattern_matches(p, tag) for p in patterns):
            problems.append(f"tag {tag!r} does NOT fire the release workflow "
                            f"(patterns: {patterns})")

    for ref in NON_TAGS:
        hit = [p for p in patterns if pattern_matches(p, ref)]
        if hit:
            problems.append(
                f"non-tag ref {ref!r} DOES fire the release workflow via "
                f"{hit} -- a push to it cannot release, so the run fails in "
                f"preflight; see the note on quantifiers in release.yml")

    return problems


def self_test() -> int:
    failures: list[str] = []

    def expect(cond: bool, what: str) -> None:
        print(("PASS  " if cond else "FAIL  ") + what)
        if not cond:
            failures.append(what)

    # 1. ECMAScript quantifier semantics: `*` and `+` bind to the *class*,
    #    so `v[0-9]*` already means "v, then any run of digits" and a `+`
    #    on top of it is a second quantifier applied to the same atom.
    #    Python's `re` refuses this shape; ECMAScript accepts it.  This is
    #    the reason the engine is Node and not `re`.
    expect(pattern_matches("v[0-9]*", "v") and pattern_matches("v[0-9]*", "v123"),
           "`v[0-9]*` is 'v then zero or more digits' (ECMAScript semantics)")

    # 2. GitHub anchors a filter at the *start* of the ref, not at the end:
    #    an unanchored pattern is a prefix test, and a prefix test passes a
    #    branch whose name begins with a version-shaped string.  This is the
    #    second half of the defect (the first is quantifier binding), and it
    #    is why the shipped patterns end in `$`.
    expect(pattern_matches("v[0-9]*.[0-9]*.[0-9]*",
                           "integrate/q3-x") is False
           and pattern_matches("v[0-9]*.[0-9]*.[0-9]*",
                               "v1.2.0-post-release") is True,
           "an unanchored pattern is a prefix test (matches a tag *prefix*)")

    # 3. The shipped filter matches every tag form and no branch.
    patterns = read_patterns(WORKFLOW.read_text(encoding="utf-8"))
    expect(bool(patterns), f"release.yml declares on.push.tags ({patterns})")
    problems = check(patterns)
    for p in problems:
        print("        " + p)
    expect(not problems, "the release.yml filter matches every tag form and "
                         "no branch name")

    # 3. A filter that is too narrow is rejected.
    expect(bool(check(["v1.2.0"])),
           "check() rejects a filter that matches only one exact tag")

    #    A permissive-but-plausible filter that matches a real branch:
    #    `release/` is not tag-shaped, but a filter author widening the
    #    version span would write exactly this.  This proves the branch leg
    #    of check() with a filter a reviewer might actually write -- not
    #    with `*`, which would match everything and prove nothing.
    permissive = patterns + ["r.*"]
    expect(any(pattern_matches("r.*", b) for b in NON_TAGS)
           and bool(check(permissive)),
           "check() rejects a filter that matches a branch name")

    if failures:
        print(f"\n{len(failures)} self-test failure(s).")
        return 1
    print("\nPASS -- the self-test confirms every classification.")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true",
                        help="run the built-in checks and exit")
    parser.add_argument("--workflow", default=str(WORKFLOW),
                        help="path to the release workflow (default: the repo's)")
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()

    path = Path(args.workflow)
    if not path.is_file():
        print(f"error: {path} does not exist", file=sys.stderr)
        return 2
    patterns = read_patterns(path.read_text(encoding="utf-8"))
    if not patterns:
        print(f"error: no on.push.tags patterns in {path}", file=sys.stderr)
        return 2

    try:
        problems = check(patterns)
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    for p in problems:
        print("FAIL  " + p)
    if problems:
        return 1
    print(f"PASS -- {len(patterns)} pattern(s) match {len(TAGS)} tag form(s) "
          f"and none of {len(NON_TAGS)} non-tag ref(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
