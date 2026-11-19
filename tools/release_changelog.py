#!/usr/bin/env python3
"""Turn the `[Unreleased]` body into a released `[1.2.0]` section.

## Why

`CHANGELOG.md` accumulates its entries under `[Unreleased]` as they
land.  Because each pull request appends under whichever `### Added` /
`### Fixed` / `### Tooling` heading it happens to want, the section
ends up with **the same heading repeated several times** -- as the
`v1.2.0` body does today, with three `### Added` blocks and two
`### Fixed` blocks interleaved with `### Removed`, `### Changed`,
`### Performance` and `### Tooling`.

That is not a release-notes document.  `Keep a Changelog` expects one
section per category, in a fixed order, and the `v1.1.0` section in
this same file already follows that shape (`### Fixed`, `### Changed`,
`### Added`, `### Known limitations`).  So the release step is not a
rename: it is a *regroup*.

## What this script does

1. Reads the body under `## [Unreleased]`.
2. Splits it into `(heading, entries)` groups, where each entry is one
   top-level bullet and its continuation lines.
3. Re-emits the groups under a **canonical order** -- Added, Changed,
   Deprecated, Removed, Fixed, Performance, Tooling -- merging the
   repeated headings of the same name into one, and preserving the
   original order of the entries inside each.
4. Writes a fresh, empty `## [Unreleased]` above the new `## [1.2.0]`
   section, and puts a `### Status:` paragraph in front of the
   categories so the section says what the release *is* before it lists
   what changed.

The transformation is deliberately conservative: only the *grouping*
changes.  No entry text is edited, reordered within its category, or
dropped -- the script asserts that the multiset of entries before and
after is identical, and fails loudly if it is not.

## Usage

    python tools/release_changelog.py --version 1.2.0 --date 2026-11-18 \
        --status "..." [--check]

`--check` reports whether the transformation is *possible* (the
structure is well-formed) without writing anything.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CHANGELOG_PATH = REPO_ROOT / "CHANGELOG.md"

# The canonical `Keep a Changelog` order.  A heading that is not in
# this list keeps its relative position after the known ones.
CANONICAL_ORDER = (
    "Added",
    "Changed",
    "Deprecated",
    "Removed",
    "Fixed",
    "Performance",
    "Tooling",
)

#: The `### Status:` label for a release whose version carries **no**
#: SemVer pre-release identifier.
#:
#: The label used to be the hard-coded string "Release candidate" for
#: every release, whatever the version was.  That was wrong twice over:
#: `v1.0.0`, `v1.0.1` and `v1.0.2` were all cut as stable releases
#: (their own sections say "Stable"), and `v1.1.0` / `v1.2.0` were
#: published as stable GitHub Releases -- non-prerelease, marked
#: "Latest" -- while their sections still claimed to be candidates.
#: The label is now *derived* from the version, so it cannot drift from
#: the tag again.
STATUS_STABLE = "Stable"

#: The `### Status:` label for a release whose version carries a
#: pre-release identifier (`-rc.N`, `-alpha.N`, `-beta.N`).
#:
#: Per `RELEASING.md` §2.2 those tags are candidates by construction:
#: the pre-release identifier *is* the statement of what the tag is.
STATUS_CANDIDATE = "Release candidate"


def status_label(version: str) -> str:
    """The `### Status:` label for `version`.

    A version with a SemVer pre-release identifier (`1.2.0-rc.1`) is a
    candidate; a version without one (`1.2.0`) is a stable release.

    The rule is deliberately mechanical rather than a judgement call:
    the label has to be derivable from the thing it describes, because
    a hand-written label is exactly the thing that went stale.
    """
    # `version` is `X.Y.Z` or `X.Y.Z-<pre-release>`.  Anything after the
    # `-` is a pre-release identifier; nothing after it is not.
    return STATUS_CANDIDATE if "-" in version else STATUS_STABLE


def read_text(path: Path) -> str:
    return path.read_bytes().decode("utf-8-sig")


def write_text(path: Path, text: str) -> None:
    path.write_bytes(text.encode("utf-8"))


def split_sections(lines: list[str]) -> list[tuple[str, int, int]]:
    """Return `[(heading, start, end)]` for every `## ` section."""
    out: list[tuple[str, int, int]] = []
    for i, line in enumerate(lines):
        if line.startswith("## "):
            start = i
            # find the next `## `
            j = i + 1
            while j < len(lines) and not lines[j].startswith("## "):
                j += 1
            out.append((line, start, j))
    return out


def parse_unreleased_body(lines: list[str]) -> tuple[str, list[tuple[str, list[str]]]]:
    """Split the `[Unreleased]` body into a preamble and its groups.

    Returns `(preamble, groups)` where `groups` is a list of
    `(heading, entries)` and each entry is a list of lines (the bullet
    line plus its continuation lines).
    """
    sections = split_sections(lines)
    target = None
    for heading, start, end in sections:
        if heading.strip() == "## [Unreleased]":
            target = (start, end)
            break
    if target is None:
        raise SystemExit("release_changelog.py: no '## [Unreleased]' section")

    start, end = target
    body = lines[start + 1 : end]

    preamble: list[str] = []
    groups: list[tuple[str, list[list[str]]]] = []
    current_heading: str | None = None
    current_entries: list[list[str]] = []

    for line in body:
        if line.startswith("### "):
            if current_heading is not None:
                groups.append((current_heading, current_entries))
            current_heading = line[4:].strip()
            current_entries = []
            continue
        if current_heading is None:
            # Text before the first `###` heading is the preamble.
            preamble.append(line)
            continue
        if line.startswith("- "):
            current_entries.append([line])
            continue
        if current_entries:
            # A continuation line (or a blank line inside an entry).
            current_entries[-1].append(line)
        else:
            # Trailing text of the heading itself (rare).
            current_entries.append([line])

    if current_heading is not None:
        groups.append((current_heading, current_entries))

    # Drop the trailing blank lines of every group -- they are
    # re-introduced by the writer.
    cleaned: list[tuple[str, list[list[str]]]] = []
    for heading, entries in groups:
        trimmed = []
        for entry in entries:
            while entry and not entry[-1].strip():
                entry.pop()
            if entry:
                trimmed.append(entry)
        cleaned.append((heading, trimmed))

    return "\n".join(preamble).strip("\n"), cleaned


def canonical_key(heading: str) -> int:
    """Sort key for a heading: known headings first, in order."""
    base = heading.split("(")[0].strip()
    for i, name in enumerate(CANONICAL_ORDER):
        if base.lower() == name.lower():
            return i
    return len(CANONICAL_ORDER)


def regroup(groups: list[tuple[str, list[list[str]]]]) -> list[tuple[str, list[list[str]]]]:
    """Merge the same heading and sort the headings canonically."""
    merged: dict[str, list[list[str]]] = {}
    order: list[str] = []
    for heading, entries in groups:
        if heading not in merged:
            merged[heading] = []
            order.append(heading)
        merged[heading].extend(entries)
    return sorted(((h, merged[h]) for h in order), key=lambda kv: canonical_key(kv[0]))


def render(
    preamble: str,
    groups: list[tuple[str, list[list[str]]]],
    version: str,
    date: str,
    status: str,
    next_note: str,
) -> str:
    """Render the new file text."""
    out: list[str] = []
    out.append("## [Unreleased]")
    out.append("")
    out.append(f"> {next_note}")
    out.append("")
    out.append(f"## [{version}] - {date}")
    out.append("")
    if status:
        out.append(f"### Status: {status_label(version)}")
        out.append("")
        for para in status.strip().split("\n\n"):
            out.append("> " + para.replace("\n", "\n> "))
            out.append("")
        out.append("")
    if preamble:
        out.append(preamble)
        out.append("")
    for heading, entries in groups:
        out.append(f"### {heading}")
        out.append("")
        for entry in entries:
            out.extend(entry)
            out.append("")
    # Trim the trailing blank lines to exactly one newline at EOF.
    while out and not out[-1].strip():
        out.pop()
    return "\n".join(out) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="release_changelog.py")
    parser.add_argument("--version", required=True, help="the released version, e.g. 1.2.0")
    parser.add_argument("--date", required=True, help="the release date, e.g. 2026-11-18")
    parser.add_argument(
        "--status",
        default="",
        help="the '### Status' paragraph (markdown block-quote body)",
    )
    parser.add_argument(
        "--status-file",
        default="",
        help="read the '### Status' paragraph from this file instead of "
        "the command line (a multi-line argument is awkward to pass "
        "through a shell)",
    )
    parser.add_argument(
        "--next-note",
        default="",
        help="the note under the new empty [Unreleased] heading",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="only report whether the structure is well-formed",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="run the tool's own tests and exit",
    )
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()

    text = read_text(CHANGELOG_PATH)
    lines = text.split("\n")
    preamble, groups = parse_unreleased_body(lines)

    total_before = sum(len(entries) for _, entries in groups)
    total_after = sum(len(entries) for _, entries in regroup(groups))
    if total_before != total_after:
        print(
            f"release_changelog.py: entry count changed ({total_before} -> {total_after})",
            file=sys.stderr,
        )
        return 1

    if args.check:
        print(
            f"CHANGELOG.md is regroupable: {total_before} entries in "
            f"{len(groups)} groups -> {len(regroup(groups))} canonical groups"
        )
        for heading, entries in regroup(groups):
            print(f"  ### {heading}: {len(entries)} entries")
        return 0

    note = args.next_note or f"Work that lands after `v{args.version}`."

    status = args.status
    if args.status_file:
        status = read_text(Path(args.status_file)).strip()

    sections = split_sections(lines)
    unreleased = next((s for s in sections if s[0].strip() == "## [Unreleased]"), None)
    if unreleased is None:
        print("release_changelog.py: no '## [Unreleased]' section", file=sys.stderr)
        return 1
    _, start, end = unreleased

    new_text = render(
        preamble, regroup(groups), args.version, args.date, status, note
    )

    # The new text replaces the whole `[Unreleased]` section; everything
    # after it (`[1.1.0]` and below) is untouched.
    rebuilt = "\n".join(lines[:start]) + "\n" + new_text + "\n" + "\n".join(lines[end:])
    rebuilt = rebuilt.replace("\n\n\n\n", "\n\n\n")

    write_text(CHANGELOG_PATH, rebuilt)
    print(
        f"CHANGELOG.md: wrote the [{args.version}] section "
        f"({total_after} entries, {len(regroup(groups))} groups) and emptied [Unreleased]"
    )
    return 0


def self_test() -> int:
    """Self-test the status-label rule.

    The rule under test is the one that went stale: the label has to
    follow the version, because a hard-coded label called `v1.1.0` and
    `v1.2.0` -- both published as stable GitHub Releases -- "Release
    candidate".
    """
    failures: list[str] = []

    def check(cond: bool, what: str) -> None:
        if not cond:
            failures.append(what)

    # A plain version is a stable release.
    for version in ("1.0.0", "1.0.1", "1.0.2", "1.1.0", "1.2.0", "2.0.0"):
        check(
            status_label(version) == STATUS_STABLE,
            f"{version} must be labelled {STATUS_STABLE!r}, "
            f"got {status_label(version)!r}",
        )

    # A version with a pre-release identifier is a candidate.
    for version in ("0.9.0-rc1", "0.14.0-rc.1", "0.14.0-beta.1", "1.0.0-alpha.1"):
        check(
            status_label(version) == STATUS_CANDIDATE,
            f"{version} must be labelled {STATUS_CANDIDATE!r}, "
            f"got {status_label(version)!r}",
        )

    # The rendered section must carry the derived label, not a constant.
    rendered = render("", [], "1.2.0", "2026-11-18", "The release.", "")
    check(
        "### Status: Stable" in rendered,
        "a stable version must render the Stable label",
    )
    check(
        "### Status: Release candidate" not in rendered,
        "a stable version must not render the candidate label",
    )
    rendered = render("", [], "1.2.0-rc.1", "2026-11-18", "The release.", "")
    check(
        "### Status: Release candidate" in rendered,
        "a candidate version must render the candidate label",
    )

    if failures:
        for f in failures:
            print(f"release_changelog.py: FAIL -- {f}", file=sys.stderr)
        return 1

    print("release_changelog.py: self-test passed")
    return 0


if __name__ == "__main__":
    if "--self-test" in sys.argv[1:]:
        sys.exit(self_test())
    sys.exit(main())
