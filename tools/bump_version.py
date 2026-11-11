#!/usr/bin/env python3
"""Single-point version control for NimRTC.

## Why this script exists

The version of the project lives in three places that have to agree:

  1. `src/core/include/nimrtc/core/api_version.hpp` -- the public API
     contract.  `NIMRTC_API_VERSION_MAJOR/MINOR/PATCH/PRE_RELEASE` are
     what a consumer compiles against (ADR-015).  This is the single
     source of truth.
  2. `CMakeLists.txt` -- `project(nimrtc VERSION X.Y.Z)`, which is what
     `find_package(NimRTC)` and the packaging rules read.
  3. `tests/api_version_expectations.hpp` -- the pinned expectation the
     version smoke test compares against, so that an *accidental*
     version change cannot slip through unnoticed.

Historically the three were edited by hand, one at a time.  The
post-`v1.1.0` bump (`059cc29`) changed the header and `CMakeLists.txt`
but not the test, and the resulting red CI surfaced as a `static_assert`
failure in four platform builds plus a freeze-checklist failure -- a
confusing way to learn that a version was bumped in the wrong order.
The remedy is not "remember to edit three files", it is "make the
machine do it", and make the mismatch cheap to detect.

## Usage

    python tools/bump_version.py --show
    python tools/bump_version.py --set 1.1.1 --pre-release dev
    python tools/bump_version.py --bump patch --pre-release dev
    python tools/bump_version.py --check

`--check` is the CI gate: it exits non-zero and names every file that
drifted.  It is fast (no build, no network) and belongs in the
`docs` job so a drift is reported in seconds instead of after four
platform builds.

`--set`/`--bump` are the only supported way to change the version.
They rewrite all three sites together, so the tree is never observed
in a half-bumped state.

## The pre-release label

`NIMRTC_API_VERSION_PRE_RELEASE` follows the cycle documented in
`RELEASING.md` and in the header itself:

    "dev" -> "alpha.N" -> "beta.N" -> "rc.N" -> ""

`dev` is the ordinary trunk state between two releases (RELEASING.md
S3.4 step 14), so `scripts/check-release-freeze.sh` accepts it in its
non-final-tag mode and only requires the label to be empty at the moment
a final tag is cut.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

HEADER_PATH = REPO_ROOT / "src/core" / "include" / "nimrtc" / "core" / "api_version.hpp"
CMAKE_PATH = REPO_ROOT / "CMakeLists.txt"
EXPECTATIONS_PATH = REPO_ROOT / "tests" / "api_version_expectations.hpp"
TEST_PATH = REPO_ROOT / "tests" / "test_api_version.cpp"


# ---------------------------------------------------------------------------
# Version model
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Version:
    """A SemVer version plus the pre-release stage label."""

    major: int
    minor: int
    patch: int
    pre_release: str = ""

    @property
    def text(self) -> str:
        """`MAJOR.MINOR.PATCH` -- what `project(VERSION)` takes."""
        return f"{self.major}.{self.minor}.{self.patch}"

    @property
    def encoded(self) -> int:
        """The `NIMRTC_API_VERSION` encoding: MAJOR*10000 + MINOR*100 + PATCH."""
        return self.major * 10000 + self.minor * 100 + self.patch

    @property
    def label(self) -> str:
        """`MAJOR.MINOR.PATCH[-pre]`, i.e. what a tag would be named."""
        return f"{self.text}-{self.pre_release}" if self.pre_release else self.text

    def describe(self) -> str:
        return f"{self.label} (encoded {self.encoded})"


def parse_version(text: str) -> Version:
    """Parse `1.2.3`, `1.2.3-dev`, `v1.2.3-rc.1`."""
    cleaned = text.strip()
    if cleaned.startswith("v"):
        cleaned = cleaned[1:]

    pre = ""
    if "-" in cleaned:
        cleaned, pre = cleaned.split("-", 1)

    parts = cleaned.split(".")
    if len(parts) != 3:
        raise ValueError(f"not a MAJOR.MINOR.PATCH version: {text!r}")
    try:
        major, minor, patch = (int(part) for part in parts)
    except ValueError as exc:
        raise ValueError(f"not a MAJOR.MINOR.PATCH version: {text!r}") from exc

    return Version(major, minor, patch, pre)


def label_description(pre_release: str) -> str:
    """The prose the header's doc comment uses for a given stage label."""
    if pre_release == "":
        return "release"
    if pre_release == "dev":
        return "development cycle"
    return f"{pre_release} cycle"


def label_argument(version: Version) -> str:
    """The `--pre-release` argument that selects this version's label."""
    return version.pre_release if version.pre_release else '""'


# ---------------------------------------------------------------------------
# File helpers
# ---------------------------------------------------------------------------


def read_text(path: Path) -> tuple[str, bool]:
    """Return `(text, had_bom)` for a UTF-8 file, tolerating a BOM."""
    raw = path.read_bytes()
    had_bom = raw.startswith(b"\xef\xbb\xbf")
    return raw.decode("utf-8-sig"), had_bom


def write_text(path: Path, text: str, had_bom: bool) -> None:
    """Write UTF-8 text back, preserving the BOM state of the original."""
    data = text.encode("utf-8")
    if had_bom:
        data = b"\xef\xbb\xbf" + data
    path.write_bytes(data)


def _sub_once(pattern: str, text: str, replacement, what: str) -> str:
    regex = re.compile(pattern, re.M)
    new_text, count = regex.subn(replacement, text, count=1)
    if count != 1:
        raise SystemExit(
            f"bump_version.py: could not locate {what} (pattern {pattern!r}); "
            "the file layout changed and this script needs updating"
        )
    return new_text


# ---------------------------------------------------------------------------
# The three sites
# ---------------------------------------------------------------------------


def parse_header(text: str) -> Version:
    """Read the version out of `api_version.hpp`."""
    values = {}
    for name, key in (
        ("NIMRTC_API_VERSION_MAJOR", "major"),
        ("NIMRTC_API_VERSION_MINOR", "minor"),
        ("NIMRTC_API_VERSION_PATCH", "patch"),
    ):
        match = re.search(
            rf"^#define\s+{re.escape(name)}\s+(\d+)\s*$", text, re.M
        )
        if not match:
            raise SystemExit(
                f"bump_version.py: {name} not found in {HEADER_PATH.relative_to(REPO_ROOT)}"
            )
        values[key] = int(match.group(1))

    match = re.search(
        r'^#define\s+NIMRTC_API_VERSION_PRE_RELEASE\s+"([^"]*)"\s*$', text, re.M
    )
    if not match:
        raise SystemExit(
            "bump_version.py: NIMRTC_API_VERSION_PRE_RELEASE not found in "
            f"{HEADER_PATH.relative_to(REPO_ROOT)}"
        )
    values["pre_release"] = match.group(1)

    return Version(**values)


def render_header(text: str, version: Version) -> str:
    """Return `text` with the version macros and their doc comments updated."""
    for name, value in (
        ("NIMRTC_API_VERSION_MAJOR", str(version.major)),
        ("NIMRTC_API_VERSION_MINOR", str(version.minor)),
        ("NIMRTC_API_VERSION_PATCH", str(version.patch)),
        ("NIMRTC_API_VERSION_PRE_RELEASE", f'"{version.pre_release}"'),
    ):
        text = _sub_once(
            rf"^(#define\s+{re.escape(name)}\s+).*?(\s*)$",
            text,
            lambda match, v=value: match.group(1) + v + match.group(2),
            name,
        )

    # The doc comment above PRE_RELEASE names the version and the stage.
    text = _sub_once(
        r"^/// Pre-release identifier for the v[0-9]+\.[0-9]+\.[0-9]+ [^\n]*\n"
        r"/// final releases\.$",
        text,
        lambda _match: (
            f"/// Pre-release identifier for the v{version.text} "
            f"{label_description(version.pre_release)}.  Empty string for\n"
            "/// final releases."
        ),
        "the NIMRTC_API_VERSION_PRE_RELEASE doc comment",
    )

    # The doc comment above the encoded integer names the version.
    text = _sub_once(
        r"^/// Format: MAJOR\*10000 \+ MINOR\*100 \+ PATCH\.  "
        r"v[0-9]+\.[0-9]+\.[0-9]+ = [0-9]+\.$",
        text,
        lambda _match: (
            "/// Format: MAJOR*10000 + MINOR*100 + PATCH.  "
            f"v{version.text} = {version.encoded}."
        ),
        "the NIMRTC_API_VERSION doc comment",
    )

    return text


def render_cmake(text: str, version: Version) -> str:
    """Return `text` with `project(nimrtc VERSION ...)` updated."""
    return _sub_once(
        r"(project\(\s*nimrtc\s*\n\s*VERSION\s+)[0-9]+\.[0-9]+\.[0-9]+",
        text,
        lambda match: match.group(1) + version.text,
        "project(nimrtc VERSION ...)",
    )


def parse_cmake(text: str) -> str:
    """Read `MAJOR.MINOR.PATCH` out of `CMakeLists.txt`."""
    match = re.search(
        r"project\(\s*nimrtc\s*\n\s*VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)", text, re.M
    )
    if not match:
        raise SystemExit(
            "bump_version.py: project(nimrtc VERSION ...) not found in CMakeLists.txt"
        )
    return match.group(1)


def render_expectations(version: Version) -> str:
    """Render the pinned expectation header the smoke test includes."""
    return f"""// Generated by tools/bump_version.py -- DO NOT EDIT BY HAND.
//
// These constants pin the public API version that the release process has
// announced.  `tests/test_api_version.cpp` compares them against the macros
// in <nimrtc/core/api_version.hpp>: a mismatch means the version was
// changed without going through `tools/bump_version.py`, which is exactly
// the failure this file exists to catch.
//
// Regenerate with:
//     python tools/bump_version.py --set {version.text} --pre-release {label_argument(version)}

#ifndef NIMRTC_TESTS_API_VERSION_EXPECTATIONS_HPP
#define NIMRTC_TESTS_API_VERSION_EXPECTATIONS_HPP

#define NIMRTC_EXPECTED_API_VERSION_MAJOR {version.major}
#define NIMRTC_EXPECTED_API_VERSION_MINOR {version.minor}
#define NIMRTC_EXPECTED_API_VERSION_PATCH {version.patch}
#define NIMRTC_EXPECTED_API_VERSION_PRE_RELEASE "{version.pre_release}"
#define NIMRTC_EXPECTED_API_VERSION_ENCODED {version.encoded}
#define NIMRTC_EXPECTED_API_VERSION_STRING "{version.text}"

#endif  // NIMRTC_TESTS_API_VERSION_EXPECTATIONS_HPP
"""


def parse_expectations(text: str) -> Version:
    """Read the pinned version out of the generated expectations header."""
    values = {}
    for name, key in (
        ("NIMRTC_EXPECTED_API_VERSION_MAJOR", "major"),
        ("NIMRTC_EXPECTED_API_VERSION_MINOR", "minor"),
        ("NIMRTC_EXPECTED_API_VERSION_PATCH", "patch"),
    ):
        match = re.search(rf"^#define\s+{re.escape(name)}\s+(\d+)\s*$", text, re.M)
        if not match:
            raise SystemExit(
                f"bump_version.py: {name} not found in "
                f"{EXPECTATIONS_PATH.relative_to(REPO_ROOT)}"
            )
        values[key] = int(match.group(1))

    match = re.search(
        r'^#define\s+NIMRTC_EXPECTED_API_VERSION_PRE_RELEASE\s+"([^"]*)"\s*$', text, re.M
    )
    if not match:
        raise SystemExit(
            "bump_version.py: NIMRTC_EXPECTED_API_VERSION_PRE_RELEASE not found in "
            f"{EXPECTATIONS_PATH.relative_to(REPO_ROOT)}"
        )
    values["pre_release"] = match.group(1)

    return Version(**values)


# ---------------------------------------------------------------------------
# Actions
# ---------------------------------------------------------------------------


def current_version() -> Version:
    return parse_header(read_text(HEADER_PATH)[0])


def set_version(version: Version, dry_run: bool) -> list[str]:
    """Rewrite every site to `version`.  Returns the list of changed files."""
    changed: list[str] = []

    for path, render in (
        (HEADER_PATH, render_header),
        (CMAKE_PATH, render_cmake),
    ):
        text, had_bom = read_text(path)
        updated = render(text, version)
        if updated != text:
            changed.append(str(path.relative_to(REPO_ROOT)))
            if not dry_run:
                write_text(path, updated, had_bom)

    if EXPECTATIONS_PATH.exists():
        text, had_bom = read_text(EXPECTATIONS_PATH)
    else:
        text, had_bom = "", False
    updated = render_expectations(version)
    if updated != text:
        changed.append(str(EXPECTATIONS_PATH.relative_to(REPO_ROOT)))
        if not dry_run:
            write_text(EXPECTATIONS_PATH, updated, had_bom)

    return changed


def check() -> int:
    """Verify every site agrees with the header.  Returns a process exit code."""
    problems: list[str] = []

    header_text, _ = read_text(HEADER_PATH)
    version = parse_header(header_text)

    # 1. The header must be a fixed point: its doc comments must already
    #    describe the version the macros declare.
    expected_header = render_header(header_text, version)
    if expected_header != header_text:
        problems.append(
            f"{HEADER_PATH.relative_to(REPO_ROOT)}: the doc comments do not "
            f"describe {version.describe()}"
        )

    # 2. CMake must declare the same version.
    cmake_text, _ = read_text(CMAKE_PATH)
    cmake_version = parse_cmake(cmake_text)
    if cmake_version != version.text:
        problems.append(
            f"{CMAKE_PATH.relative_to(REPO_ROOT)}: project(VERSION) is "
            f"{cmake_version}, the API version is {version.text}"
        )

    # 3. The pinned expectations must match the header.
    if not EXPECTATIONS_PATH.exists():
        problems.append(
            f"{EXPECTATIONS_PATH.relative_to(REPO_ROOT)}: missing; run "
            f"`python tools/bump_version.py --set {version.text} "
            f"--pre-release {label_argument(version)}`"
        )
    else:
        expectations_text, _ = read_text(EXPECTATIONS_PATH)
        expected = render_expectations(version)
        if expectations_text != expected:
            pinned = parse_expectations(expectations_text)
            problems.append(
                f"{EXPECTATIONS_PATH.relative_to(REPO_ROOT)}: pins "
                f"{pinned.describe()}, the API version is {version.describe()}"
            )

    # 4. The smoke test must read the pinned expectations rather than
    #    hardcoding a version of its own.
    if not TEST_PATH.exists():
        problems.append(f"{TEST_PATH.relative_to(REPO_ROOT)}: missing")
    else:
        test_text, _ = read_text(TEST_PATH)
        if "api_version_expectations.hpp" not in test_text:
            problems.append(
                f"{TEST_PATH.relative_to(REPO_ROOT)}: does not include "
                "api_version_expectations.hpp"
            )
        if f'"{version.text}"' in test_text:
            problems.append(
                f"{TEST_PATH.relative_to(REPO_ROOT)}: hardcodes the version "
                f"string \"{version.text}\"; it must use "
                "NIMRTC_EXPECTED_API_VERSION_* instead"
            )

    if problems:
        print("version consistency check FAILED", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        print(
            "\nFix with: python tools/bump_version.py --set "
            f"{version.text} --pre-release {label_argument(version)}",
            file=sys.stderr,
        )
        return 1

    print(f"version consistency check PASSED ({version.describe()})")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="bump_version.py",
        description="Single-point version control for NimRTC.",
    )
    group = parser.add_mutually_exclusive_group()
    group.add_argument(
        "--show",
        action="store_true",
        help="print the current version and exit",
    )
    group.add_argument(
        "--check",
        action="store_true",
        help="verify every version site agrees; exit non-zero on drift",
    )
    group.add_argument(
        "--set",
        dest="set_to",
        metavar="VERSION",
        help="set the version, e.g. 1.1.1 or 1.1.1-rc.1",
    )
    group.add_argument(
        "--bump",
        choices=("major", "minor", "patch"),
        help="increment the given component of the current version",
    )
    parser.add_argument(
        "--pre-release",
        metavar="LABEL",
        default=None,
        help="the pre-release stage label: dev, alpha.N, beta.N, rc.N, or "
        '"" for a final release',
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="report what would change without writing anything",
    )
    args = parser.parse_args(argv)

    if args.show:
        version = current_version()
        print(version.text)
        print(version.pre_release)
        print(version.encoded)
        return 0

    if args.check:
        return check()

    if args.set_to is not None:
        version = parse_version(args.set_to)
        if args.pre_release is not None:
            version = Version(
                version.major, version.minor, version.patch, args.pre_release
            )
    elif args.bump is not None:
        current = current_version()
        if args.bump == "major":
            version = Version(current.major + 1, 0, 0, "")
        elif args.bump == "minor":
            version = Version(current.major, current.minor + 1, 0, "")
        else:
            version = Version(current.major, current.minor, current.patch + 1, "")
        if args.pre_release is not None:
            version = Version(
                version.major, version.minor, version.patch, args.pre_release
            )
    else:
        parser.print_help()
        return 2

    changed = set_version(version, dry_run=args.dry_run)
    prefix = "would update" if args.dry_run else "updated"
    if changed:
        for path in changed:
            print(f"{prefix}: {path}")
    else:
        print(f"nothing to do -- the tree is already at {version.describe()}")
    print(f"version is now {version.describe()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
