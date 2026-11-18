#!/usr/bin/env python3
"""Build a GitHub release body that fits inside GitHub's size limit.

## Why this exists

The `v1.2.0` release cut failed.  Every artefact built, every asset
uploaded, and then the publish step died with:

    gh: Validation Failed (HTTP 422)

The cause is the **release body**, not the assets.  `release.yml`
extracts the `## [X.Y.Z]` section of `CHANGELOG.md` verbatim and hands
it to GitHub as the release body -- and GitHub caps a release body at
**125 000 characters**.  The `v1.2.0` section is **364 350 characters**:
the E8 release-note regroup put all 217 accumulated entries into one
section, and the section grew past what GitHub accepts.

The failure mode is the dangerous kind.  GitHub does not reject the
over-long body loudly at the point of the mistake -- it *silently
truncates* it.  The draft release that survived carries 124 999
characters of the 364 350, ending mid-sentence in the middle of an
entry.  Anyone reading the published release page would see a document
that simply stops, with no indication that the rest exists.

(`v1.1.0` was 44 143 characters, so this never fired before.)

## What this script does

The full release section is the **preferred** body: it is the complete
notes, and every release so far has fit inside GitHub's limit.  So the
script emits the section *whole* whenever it fits.

Only when the section is too large does it fall back to a **digest**:

- the `### Status:` paragraph is kept **whole** -- it is the part that
  says what the release *is*, and it is short;
- every other category's entries are reduced to their **bold
  headline** -- the one-sentence statement of what changed, with the
  prose and the code blocks left behind;
- a footer points at `CHANGELOG.md` for the full text.

For `v1.2.0` the digest is ~24 000 characters against a 125 000 limit:
a document a human can actually read, with nothing lost, because the
complete release notes live in `CHANGELOG.md` in the repository and on
the tag.

The script **fails** if even the digest would exceed the limit, rather
than emitting a body that GitHub will truncate.  A silent truncation
is the failure this exists to prevent, so it must not be able to happen
again quietly.

## Usage

    python tools/release_body.py --version 1.2.0 --out dist/release_notes.md
    python tools/release_body.py --version 1.2.0 --check

`--check` writes the body to stdout and verifies the length without
touching any file.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CHANGELOG_PATH = REPO_ROOT / "CHANGELOG.md"

#: GitHub's hard limit on a release body, in characters.
#:
#: The API rejects a longer body with `422 Validation Failed`, and the
#: web UI truncates one silently.  Both were observed on the `v1.2.0`
#: cut; the silent truncation is the reason this script exists.
GITHUB_BODY_LIMIT = 125_000

#: The safety margin below the hard limit.
#:
#: The body the workflow finally posts is the digest *plus* the
#: signing-status footer and the per-platform 3A status, both of which
#: are appended after this script runs.  The margin leaves room for
#: them without having to know their size in advance.
SAFETY_MARGIN = 8_000

#: The limit this script enforces: the hard limit minus the margin.
MAX_BODY_CHARS = GITHUB_BODY_LIMIT - SAFETY_MARGIN

#: The bullet's bold headline, e.g. `- **The thing.** Prose...`.
#:
#: The pattern is matched against the entry's *first* logical line
#: (the bullet plus its continuation lines, joined), because a headline
#: that wraps in the source is still one bold span.
_HEADLINE_RE = re.compile(r"^\s*-\s+\*\*(.+?)\*\*", re.DOTALL)

#: The category headings that are copied whole rather than digested.
#:
#: `Status:` is the release's own statement of what it is -- the gate
#: table, the known limitations.  It is short, it is the most valuable
#: part of the document, and digesting it would leave a reader with a
#: list of changes and no idea what they are changes *to*.
VERBATIM_GROUPS = ("Status:",)


def read_text(path: Path) -> str:
    return path.read_bytes().decode("utf-8-sig")


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(text.encode("utf-8"))


def find_section(lines: list[str], version: str) -> tuple[int, int]:
    """The `[start, end)` line range of the `## [version]` section."""
    wanted = f"## [{version}]"
    start = None
    for i, line in enumerate(lines):
        if line.startswith(wanted):
            start = i
            break
    if start is None:
        raise SystemExit(f"release_body.py: no '{wanted}' section in CHANGELOG.md")
    end = len(lines)
    for i in range(start + 1, len(lines)):
        if lines[i].startswith("## "):
            end = i
            break
    return start, end


def split_groups(lines: list[str]) -> list[tuple[str, list[list[str]]]]:
    """Split a section into `(heading, entries)`.

    An entry is one bullet plus its continuation lines.
    """
    groups: list[tuple[str, list[list[str]]]] = []
    for line in lines:
        if line.startswith("### "):
            groups.append((line[4:].strip(), []))
        elif line.startswith("- ") and groups:
            groups[-1][1].append([line])
        elif groups and groups[-1][1]:
            groups[-1][1][-1].append(line)
    return groups


def entry_text(entry: list[str]) -> str:
    """The entry as one logical line (continuations joined)."""
    return re.sub(r"\s+", " ", " ".join(x.strip() for x in entry)).strip()


def headline(entry: list[str]) -> str | None:
    """The entry's bold headline, or `None` if it has none."""
    m = _HEADLINE_RE.match(entry_text(entry))
    if not m:
        return None
    return re.sub(r"\s+", " ", m.group(1)).strip()


def digest(version: str, changelog: str, limit: int | None = None) -> str:
    """Build the release body for `version`.

    The full section is returned when it fits; otherwise it is
    reduced to a digest.

    `limit` overrides the budget -- it exists so the self-test can
    exercise both modes without inventing a 100 000-character fixture.
    """
    budget = MAX_BODY_CHARS if limit is None else limit
    lines = changelog.split("\n")
    start, end = find_section(lines, version)
    section = lines[start:end]

    whole = "\n".join(section).rstrip("\n") + "\n"
    if len(whole) <= budget:
        # The complete notes fit: ship them.  No digest is needed, and
        # the reader gets every paragraph, every measurement and every
        # code reference.
        return whole

    out: list[str] = [lines[start], ""]

    for heading, entries in split_groups(section):
        if any(heading.startswith(v) for v in VERBATIM_GROUPS):
            # Kept whole: it is the release's statement of itself.
            out.append("### " + heading)
            out.append("")
            # Re-emit the group's raw body.  The heading line and the
            # bullet structure are recovered from the section text
            # directly, so the prose and the table survive intact.
            body = _group_body(section, heading)
            out.extend(body)
            out.append("")
            continue

        if not entries:
            continue

        out.append("### " + heading)
        out.append("")
        for entry in entries:
            h = headline(entry)
            if h is None:
                # No bold headline: keep the entry's first line verbatim
                # rather than dropping it.  Losing a change because its
                # prose had no bold span would be a silent loss.
                out.append(entry_text(entry))
            else:
                out.append("- " + h)
        out.append("")

    out.append("---")
    out.append("")
    out.append(
        "> This is the **digest** of the release notes: one line per change.  "
        "The full text -- every entry in prose, with its measurements, its "
        "caveats and its code references -- is in [`CHANGELOG.md`]"
        f"(https://github.com/NimRTC/nimrtc/blob/v{version}/CHANGELOG.md), "
        "under the `## ["
        + version
        + "]` heading."
    )
    out.append("")

    return "\n".join(out)


def _group_body(section: list[str], heading: str) -> list[str]:
    """The raw body of the named `### ` group, without the heading."""
    out: list[str] = []
    inside = False
    for line in section:
        if line.startswith("### "):
            if inside:
                break
            inside = line[4:].strip() == heading
            continue
        if inside:
            out.append(line)
    # Trim the leading and trailing blank lines: the caller emits its
    # own separator around the group.
    while out and not out[0].strip():
        out.pop(0)
    while out and not out[-1].strip():
        out.pop()
    return out


def self_test() -> int:
    """Self-test the digest rules against synthetic changelogs.

    The rules under test are the ones a silent truncation would hide:
    the body must fit, the `Status:` paragraph must survive whole, and
    **no entry may be dropped**.
    """
    failures: list[str] = []

    def check(cond: bool, what: str) -> None:
        if not cond:
            failures.append(what)

    # --- A changelog that fits: the whole section must be emitted. ---
    small = """## [9.9.9] - 2026-01-01

### Status: A test

> The status paragraph.

### Added

- **One.** Its prose.
- **Two.** Its prose.
"""
    body = digest("9.9.9", small)
    check("Its prose." in body, "a section that fits must be emitted whole")
    check("This is the **digest**" not in body, "a section that fits needs no digest footer")

    # --- A changelog that does not fit: the digest must kick in. ---
    # A small budget stands in for the real 125 000-character limit, so
    # the test fixture stays readable.
    fat = (
        "## [9.9.8] - 2026-01-01\n\n"
        "### Status: A test\n\n"
        "> The status paragraph, which must survive the digest whole.\n\n"
        "### Added\n\n"
        + "".join(
            f"- **Entry {i}.** {'x' * 400}\n" for i in range(200)
        )
    )
    body = digest("9.9.8", fat, limit=5000)
    check(len(body) <= 5000, f"the digest must fit ({len(body)} > 5000)")
    check(
        "The status paragraph, which must survive the digest whole." in body,
        "the Status paragraph must survive the digest whole",
    )
    check("This is the **digest**" in body, "a digest must carry the footer")
    check("xxxx" not in body, "the digest must drop the prose")
    for i in range(200):
        check(f"Entry {i}." in body, f"entry {i} was dropped from the digest")

    # --- The headline extraction must tolerate a wrapped bold span. ---
    wrapped_entry = ["- **A headline that", "  wraps across lines.** The prose."]
    check(
        headline(wrapped_entry) == "A headline that wraps across lines.",
        f"a wrapped bold span must be joined into one headline "
        f"(got {headline(wrapped_entry)!r})",
    )
    # A headline that wraps must also survive a digest.
    wrapped = (
        "## [9.9.7] - 2026-01-01\n\n"
        "### Added\n\n"
        + "".join(f"- **Entry {i}.** {'y' * 200}\n" for i in range(20))
        + "- **A headline that\n  wraps across lines.** The prose.\n"
    )
    body = digest("9.9.7", wrapped, limit=2000)
    check(
        "- A headline that wraps across lines." in body,
        "a wrapped bold span must be joined into one headline in the digest",
    )

    # --- An entry with no bold headline must not be lost. ---
    nobold = "## [9.9.6] - 2026-01-01\n\n### Added\n\n- A plain entry with no bold span.\n"
    body = digest("9.9.6", nobold, limit=120)
    check("A plain entry with no bold span." in body, "an entry with no bold span must survive")

    # --- The real changelog must produce a body that fits. ---
    real = read_text(CHANGELOG_PATH)
    for version in ("1.2.0", "1.1.0"):
        body = digest(version, real)
        check(len(body) <= MAX_BODY_CHARS, f"{version}: the body must fit")
        check(f"## [{version}]" in body, f"{version}: the section heading must be present")

    if failures:
        for f in failures:
            print(f"release_body.py: FAIL -- {f}", file=sys.stderr)
        return 1

    print("release_body.py: self-test passed")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--version", help="the version, e.g. 1.2.0")
    ap.add_argument("--out", help="write the body here")
    ap.add_argument(
        "--check",
        action="store_true",
        help="verify the length and print the body; write nothing",
    )
    ap.add_argument(
        "--self-test",
        action="store_true",
        help="run the tool's own tests and exit",
    )
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    if not args.version:
        ap.error("--version is required (or use --self-test)")

    body = digest(args.version, read_text(CHANGELOG_PATH))
    size = len(body)

    print(
        f"[release-body] {args.version}: {size} chars "
        f"(limit {MAX_BODY_CHARS}, GitHub's hard limit {GITHUB_BODY_LIMIT})",
        file=sys.stderr,
    )
    if size > MAX_BODY_CHARS:
        print(
            f"release_body.py: the digest for {args.version} is {size} chars, "
            f"over the {MAX_BODY_CHARS}-char budget "
            f"(GitHub's hard limit is {GITHUB_BODY_LIMIT}).  "
            "Shorten the CHANGELOG's bold headlines, or raise "
            "MAX_BODY_CHARS -- do NOT let GitHub truncate it.",
            file=sys.stderr,
        )
        return 1

    if args.out:
        write_text(Path(args.out), body)
        print(f"[release-body] written to {args.out}", file=sys.stderr)

    if args.check or not args.out:
        # The body carries non-ASCII (arrows, em-dashes).  `sys.stdout`
        # is the console code page on Windows (GBK here), which cannot
        # encode them -- and a `UnicodeEncodeError` here would be
        # reported as a failure of the *check*, hiding the length
        # verdict the check exists to deliver.  Reconfigure the stream
        # to UTF-8 instead of letting it raise.
        try:
            sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
        sys.stdout.write(body)
    return 0


if __name__ == "__main__":
    sys.exit(main())
