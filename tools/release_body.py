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


def digest(version: str, changelog: str) -> str:
    """Build the release body for `version`."""
    lines = changelog.split("\n")
    start, end = find_section(lines, version)
    section = lines[start:end]

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
        f"(`https://github.com/NimRTC/nimrtc/blob/v{version}/CHANGELOG.md`), "
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
    # Trim the trailing blank lines.
    while out and not out[-1].strip():
        out.pop()
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--version", required=True, help="the version, e.g. 1.2.0")
    ap.add_argument("--out", help="write the body here")
    ap.add_argument(
        "--check",
        action="store_true",
        help="verify the length and print the body; write nothing",
    )
    args = ap.parse_args()

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
