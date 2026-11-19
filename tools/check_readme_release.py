#!/usr/bin/env python3
"""check_readme_release.py -- keep the README's release claims true.

## Why this exists

The README is the first thing a reader sees, and it makes two claims
about the release: the **badge** says which version is published, and
the **"Latest release" paragraph** says what that version contains.
Both went stale for eleven days after `v1.2.0` was published: the badge
still read `v1.0.2`, the paragraph still described `v1.0.2`, the
"Current `main`" paragraph said the trunk was ahead by `v1.1` -- and
the body of the same file, four hundred lines further down, said
"these belong to the `v1.2.0` release".  The file contradicted itself,
and a reader could not tell which half was true.

Nothing caught it.  There was no check.  The drift is a *documentation*
drift, so it is invisible to the compiler and to the test suite; it
only shows up in the repository's front page, which is exactly where
nobody looks twice.

This script is the check.  It reads three sources that are supposed to
agree and fails when they do not:

  * `CMakeLists.txt` -- `project(VERSION ...)`, the source version.
  * `CHANGELOG.md` -- the newest released section and the
    `### Status:` label it carries.
  * `README.md` / `README.zh.md` -- the badge and the "Latest release"
    paragraph.

The sources have a *relationship*, not an equality, and the check
encodes that relationship:

  * The badge names the **newest released version** -- the one with a
    `## [X.Y.Z]` section in the changelog.  It is not the source
    version: after a release is cut, `CMakeLists.txt` moves on to the
    next patch level while the newest *published* version stays where
    it was.  A badge that tracked the source version would advertise a
    release that does not exist.

  * The "Latest release" paragraph must name that same version, in
    both languages, and the changelog section for it must be labelled
    `Stable` -- a release published as non-prerelease cannot be
    labelled "Release candidate" (this was the second half of the
    same complaint).

  * The "Current `main`" paragraph must not claim the trunk is ahead
    by a version that has already been released.  That is the specific
    wording that made the README contradict itself: it said the trunk
    was ahead by `v1.1` while the same file described `v1.2.0` as
    shipped.

## What it does *not* do

It does not check prose.  It checks the version strings that a reader
would use to decide which release they are looking at.  Judging
whether a paragraph is *accurate* is not something a script can do;
what a script can do is make sure the three files do not disagree
about which version they are talking about.

## Usage

    python tools/check_readme_release.py [--repo-root ROOT] [--verbose]
    python tools/check_readme_release.py --self-test

Exit code 0 when the claims agree, 1 otherwise.

`--self-test` builds a synthetic tree with each drift shape and
asserts the checker still catches it, so the guard cannot silently
degrade into a no-op.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# The badge: `[![Version](https://img.shields.io/badge/version-v1.2.0-green.svg)](CHANGELOG.md)`
#
# The `v` is part of the badge text, not of the version, so it is
# consumed here and not captured.
BADGE_RE = re.compile(
    r"badge/version-v?([0-9][0-9A-Za-z.\-]*?)-(?:green|blue|grey|lightgrey|orange|red)\.svg")

# A changelog section heading: `## [1.2.0] - 2026-11-18` or `## [Unreleased]`.
SECTION_RE = re.compile(r"^## \[([^\]]+)\](?:\s*-\s*(\S+))?\s*$", re.MULTILINE)

# The `### Status:` label inside a section.
STATUS_RE = re.compile(r"^### Status:\s*(.+?)\s*$", re.MULTILINE)

# The English "Latest release" claim: `**Latest release:** v1.2.0 (Stable)`.
LATEST_EN_RE = re.compile(r"\*\*Latest release:\*\*\s*v?([0-9][0-9A-Za-z.\-]*)")

# The Chinese one: `**最新发布版**：v1.2.0 稳定版`.
LATEST_ZH_RE = re.compile(r"\*\*最新发布版\*\*[：:]\s*v?([0-9][0-9A-Za-z.\-]*)")

# The English "Current `main`" claim: `**Current `main` (post-v1.2.0, unreleased):**`.
CURRENT_EN_RE = re.compile(r"\*\*Current `main`\s*\((?:post-)?v?([0-9][0-9A-Za-z.\-]*)")

# The Chinese one: `**当前 `main`（v1.2.0 之后，尚未发版）**`.
CURRENT_ZH_RE = re.compile(r"\*\*当前 `main`[（(](?:v)?([0-9][0-9A-Za-z.\-]*)")

# The project version: `project(NimRTC VERSION 1.2.1)`.
PROJECT_VERSION_RE = re.compile(
    r"^\s*project\s*\(\s*[A-Za-z_][A-Za-z0-9_]*\s+VERSION\s+([0-9][0-9A-Za-z.\-]*)",
    re.MULTILINE,
)


def is_prerelease(version: str) -> bool:
    """Is this a SemVer pre-release version (`1.2.0-rc.1`)?"""
    return "-" in version


def released_sections(changelog: str) -> list[tuple[str, str]]:
    """The released sections of the changelog, newest first.

    Returns `[(version, date)]`.  The `[Unreleased]` section is not a
    release and is skipped.
    """
    out = []
    for match in SECTION_RE.finditer(changelog):
        version = match.group(1).strip()
        if version.lower() == "unreleased":
            continue
        out.append((version, (match.group(2) or "").strip()))
    return out


def section_body(changelog: str, version: str) -> str:
    """The text of the `## [version]` section."""
    start = changelog.find("## [%s]" % version)
    if start < 0:
        return ""
    rest = changelog[start + 1:]
    end = rest.find("\n## ")
    return rest[:end] if end >= 0 else rest


def version_tuple(version: str) -> tuple:
    """A sortable key for a version, ignoring any pre-release suffix."""
    core = version.split("-", 1)[0]
    parts = []
    for piece in core.split("."):
        parts.append(int(piece) if piece.isdigit() else 0)
    while len(parts) < 3:
        parts.append(0)
    return tuple(parts[:3])


class Problem:
    def __init__(self, kind: str, detail: str) -> None:
        self.kind = kind
        self.detail = detail

    def __str__(self) -> str:
        return "[%s] %s" % (self.kind, self.detail)


def check(repo_root: Path, verbose: bool = False,
          expected_tag: str | None = None) -> list[Problem]:
    problems: list[Problem] = []

    def read(rel: str) -> str:
        path = repo_root / rel
        if not path.is_file():
            raise SystemExit("error: %s not found under %s" % (rel, repo_root))
        return path.read_text(encoding="utf-8")

    cmake = read("CMakeLists.txt")
    changelog = read("CHANGELOG.md")
    readme = read("README.md")
    readme_zh = read("README.zh.md")

    # ---- the source version ------------------------------------------
    m = PROJECT_VERSION_RE.search(cmake)
    if not m:
        problems.append(Problem(
            "no-source-version",
            "CMakeLists.txt has no `project(... VERSION ...)` to read"))
        source_version = ""
    else:
        source_version = m.group(1)
        if verbose:
            print("source version: %s" % source_version)

    # ---- the newest released version ----------------------------------
    sections = released_sections(changelog)
    if not sections:
        problems.append(Problem(
            "no-released-section",
            "CHANGELOG.md has no released `## [X.Y.Z]` section"))
        return problems

    newest = sections[0][0]
    if verbose:
        print("newest released version: %s (%s)"
              % (newest, sections[0][1] or "no date"))

    # A stable release must be labelled Stable.  The label is what the
    # release body carries, and a body that says "Release candidate"
    # for a `prerelease=false` release contradicts itself.
    body = section_body(changelog, newest)
    status_match = STATUS_RE.search(body)
    if not status_match:
        problems.append(Problem(
            "no-status-label",
            "the `## [%s]` section has no `### Status:` label" % newest))
    else:
        status = status_match.group(1).strip()
        expected = "Release candidate" if is_prerelease(newest) else "Stable"
        if status != expected:
            problems.append(Problem(
                "wrong-status",
                "the `## [%s]` section says `### Status: %s`; a %s version "
                "must say `%s`" % (newest, status,
                                  "pre-release" if is_prerelease(newest)
                                  else "stable",
                                  expected)))

    # ---- the badge ----------------------------------------------------
    for label, text in (("README.md", readme), ("README.zh.md", readme_zh)):
        m = BADGE_RE.search(text)
        if not m:
            problems.append(Problem(
                "no-badge",
                "%s carries no `version-...` badge" % label))
            continue
        badge = m.group(1)
        if badge != newest:
            problems.append(Problem(
                "stale-badge",
                "%s's version badge says %s, but the newest release is %s"
                % (label, badge, newest)))

    # ---- the "Latest release" paragraph -------------------------------
    for label, text, pattern in (
            ("README.md", readme, LATEST_EN_RE),
            ("README.zh.md", readme_zh, LATEST_ZH_RE)):
        m = pattern.search(text)
        if not m:
            problems.append(Problem(
                "no-latest-release",
                "%s has no `Latest release` paragraph to check" % label))
            continue
        claimed = m.group(1)
        if claimed != newest:
            problems.append(Problem(
                "stale-latest-release",
                "%s says the latest release is %s, but the newest release "
                "is %s" % (label, claimed, newest)))

    # ---- the "Current main" paragraph ---------------------------------
    #
    # This paragraph says which version the trunk is ahead by.  It is
    # allowed to name the released version ("post-v1.2.0, unreleased")
    # -- that is the normal state after a cut.  What it must not do is
    # claim the trunk is ahead by a version that was never released,
    # which is what produced the eleven-day contradiction.
    for label, text, pattern in (
            ("README.md", readme, CURRENT_EN_RE),
            ("README.zh.md", readme_zh, CURRENT_ZH_RE)):
        m = pattern.search(text)
        if not m:
            problems.append(Problem(
                "no-current-main",
                "%s has no `Current main` paragraph to check" % label))
            continue
        claimed = m.group(1)
        if claimed not in [v for v, _ in sections]:
            problems.append(Problem(
                "unknown-current-main",
                "%s says the trunk is ahead by %s, which is not a released "
                "version" % (label, claimed)))

    # ---- the source version must not be below the newest release ------
    #
    # `CMakeLists.txt` moves to the next patch level after a cut, so
    # the source version is normally *above* the newest release.  It
    # must never be *below* it: that would mean the tree stopped
    # versioning itself.
    if source_version and sections and not is_prerelease(source_version):
        if version_tuple(source_version) < version_tuple(newest):
            problems.append(Problem(
                "source-version-behind",
                "the source version %s is behind the newest release %s"
                % (source_version, newest)))

    # ---- the tag being released must be the one the README names ------
    #
    # CI runs this check on every push, where "the newest release" is the
    # right thing to compare against.  A release cut is the opposite
    # situation: the tag that is being published is the subject, and the
    # README has to already describe it.  Without this arm, a release
    # whose README still describes the previous version passes -- the
    # three files agree with each other, and all three are stale.
    # `--expect-tag` is what preflight passes.
    if expected_tag:
        want = expected_tag[1:] if expected_tag.startswith("v") else expected_tag
        if want != newest:
            problems.append(Problem(
                "readme-not-describing-this-release",
                "the release is %s, but the README describes %s as the "
                "latest release -- update the badge and the `Latest release` "
                "paragraph in README.md and README.zh.md (and the changelog "
                "section) before cutting"
                % (expected_tag, newest)))

    return problems


# ---------------------------------------------------------------------------
# The self-test
# ---------------------------------------------------------------------------

SELF_TEST_CMAKE = """
cmake_minimum_required(VERSION 3.20)
project(NimRTC VERSION 1.2.1 LANGUAGES CXX)
"""

SELF_TEST_CHANGELOG = """
## [Unreleased]

- Work in progress.

## [1.2.0] - 2026-11-18

### Status: Stable

> The release.

## [1.1.0] - 2026-11-07

### Status: Stable

> The release.
"""

SELF_TEST_README = """
# NimRTC

[![Version](https://img.shields.io/badge/version-%s-green.svg)](CHANGELOG.md)

> **Latest release:** %s (Stable) -- the release.

> **Current `main` (post-%s, unreleased):** the trunk carries the next track.
"""

SELF_TEST_README_ZH = """
# NimRTC

[![Version](https://img.shields.io/badge/version-%s-green.svg)](CHANGELOG.md)

> **最新发布版**：%s 稳定版 —— 这个发布。

> **当前 `main`（%s 之后，尚未发版）**：主干带着下一条线。
"""


def _tree(tmp: Path, *, badge: str, latest: str, current: str,
           status: str) -> Path:
    root = tmp
    (root / ".github").mkdir(parents=True, exist_ok=True)
    (root / "CMakeLists.txt").write_text(SELF_TEST_CMAKE, encoding="utf-8")
    changelog = SELF_TEST_CHANGELOG.replace("### Status: Stable",
                                             "### Status: " + status, 1)
    (root / "CHANGELOG.md").write_text(changelog, encoding="utf-8")
    (root / "README.md").write_text(
        SELF_TEST_README % (badge, latest, current), encoding="utf-8")
    (root / "README.zh.md").write_text(
        SELF_TEST_README_ZH % (badge, latest, current), encoding="utf-8")
    return root


def self_test(tmp_root: Path) -> int:
    failures: list[str] = []

    def expect(condition: bool, message: str) -> None:
        if not condition:
            failures.append(message)

    import tempfile

    def kinds(**kwargs) -> set:
        with tempfile.TemporaryDirectory(dir=str(tmp_root)) as tmp:
            root = _tree(Path(tmp), **kwargs)
            return {p.kind for p in check(root)}

    # The consistent tree has no problems.
    expect(not kinds(badge="1.2.0", latest="1.2.0", current="1.2.0",
                     status="Stable"),
           "a consistent tree must produce no problems")

    # A stale badge is caught.
    expect("stale-badge" in kinds(badge="1.0.2", latest="1.2.0",
                                   current="1.2.0", status="Stable"),
           "a stale version badge must be caught")

    # A stale "Latest release" claim is caught.
    expect("stale-latest-release" in kinds(badge="1.2.0", latest="1.0.2",
                                           current="1.2.0", status="Stable"),
           "a stale `Latest release` paragraph must be caught")

    # A candidate label on a stable release is caught.
    expect("wrong-status" in kinds(badge="1.2.0", latest="1.2.0",
                                   current="1.2.0",
                                   status="Release candidate"),
           "a stable release labelled `Release candidate` must be caught")

    # A "Current main" paragraph naming an unreleased version is caught.
    expect("unknown-current-main" in kinds(badge="1.2.0", latest="1.2.0",
                                           current="1.9.0", status="Stable"),
           "a `Current main` claim about an unreleased version must be caught")

    # `--expect-tag` is the release-cut question: "does the README already
    # describe the tag I am publishing?"  A tag the README does not name
    # must fail even though the three files agree with each other.
    def kinds_for_tag(tag: str) -> set:
        with tempfile.TemporaryDirectory(dir=str(tmp_root)) as tmp:
            root = _tree(Path(tmp), badge="1.2.0", latest="1.2.0",
                         current="1.2.0", status="Stable")
            return {p.kind for p in check(root, expected_tag=tag)}

    expect("readme-not-describing-this-release"
           in kinds_for_tag("v1.3.0"),
           "a release whose README still describes the previous version "
           "must be caught by --expect-tag")
    expect(not kinds_for_tag("v1.2.0"),
           "a release whose README already describes it must pass "
           "--expect-tag")

    if failures:
        print("FAIL -- the self-test caught a misclassification:")
        for f in failures:
            print("  -", f)
        return 1
    print("PASS -- the self-test confirms every classification.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", default=None,
                        help="the repository root (default: inferred)")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--expect-tag", default=None,
                        help="the tag being released (e.g. v1.3.0): the "
                             "README must already describe it.  Used by "
                             "release.yml's preflight.")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            return self_test(Path(tmp))

    root = (Path(args.repo_root) if args.repo_root
            else Path(__file__).resolve().parent.parent)
    problems = check(root, args.verbose, args.expect_tag)

    if problems:
        print("FAIL -- the README's release claims disagree:")
        for p in problems:
            print("  -", p)
        return 1

    print("PASS -- the README, the changelog and the source version agree "
          "about the release.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
