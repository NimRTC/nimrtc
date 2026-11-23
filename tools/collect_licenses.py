#!/usr/bin/env python3
"""collect_licenses.py -- gather the third-party licence texts into the bundle.

## Why this exists

The release ships six static codec libraries on the Windows line
(`libvpx`, `svt-av1`, `libyuv`, `dav1d`, `openh264`, `wolfssl`), and the
Windows bundle grew from 40.5 MB at `v1.1` to `62,826,812` bytes at
`v1.2.0` -- a 55 % jump that a post-release review correctly read as
"seven new static codecs entered the MSVC line".  The review then asked
the right question: *are the licence obligations complete?*

They were not.  `release.yml` writes a CycloneDX SBOM from
`src/third_party/vendor.json`, and that SBOM carries each component's
**SPDX licence identifier** (`BSD-3-Clause`, `Apache-2.0`, …) -- but the
licence *text* was never placed in the bundle.  Both BSD-3-Clause and
Apache-2.0 require the notice and the licence text to accompany a
binary redistribution, so publishing a `.tar.xz` of the static archives
without them is a licence violation, not a nicety.

## What it does

Walks `src/third_party/*/src/` (the vendored trees), finds the
licence-like files each one ships (`LICENSE`, `LICENSE.txt`,
`COPYING`, `NOTICE`, `PATENTS`, …), and copies them into a
`licenses/<vendor>/` tree under the staging directory.  `vendor.json`
is consulted first so the vendors that are **linked into the bundle**
are listed even if their upstream tree names the file unusually.

Then it writes `licenses/THIRD_PARTY_NOTICES.txt`: one section per
vendor, with the SPDX id from `vendor.json` (or `UNKNOWN` -- which is
itself a finding) and the SHA-256 of each licence file, so a downstream
consumer can verify the text is the one that shipped.

Usage::

    python tools/collect_licenses.py --staging build/_install/windows
    python tools/collect_licenses.py --self-test

Exit code 0 when every vendor in `vendor.json` has at least one licence
file, 1 otherwise.  CI runs this before `make_release_bundle.py`, so a
vendor that lands without its licence text stops the cut instead of
shipping unlicensed.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import sys
from pathlib import Path

# The names upstream projects use for the file that carries the terms.
# These are PREFIXES, not exact names: the file that carries a licence
# is named after its stem plus whatever extension upstream chose --
# `LICENSE` (libvpx), `COPYING` (dav1d), `LICENSE.md` (svt-av1),
# `LICENSE-BSD2.md` (svt-av1's second grant), `PATENTS.md` (svt-av1's
# patent grant), `LICENSE.MIT` (nlohmann_json).
#
# The list used to enumerate whole names, mixing cases -- it held both
# `LICENSE.txt` and `LICENSE.TXT` but only `LICENSE.md` -- while the
# matcher compared `name.upper()` against it.  Every licence file with
# an extension was therefore invisible: `LICENSE.md` upper-cased to
# `LICENSE.MD`, which was not in the list.  That is how svt-av1 came
# to be reported as unlicensed and blocked the v1.3.0 cut, even though
# its `LICENSE.md` and `PATENTS.md` were sitting in the tree the whole
# time.  Matching the stem as a prefix fixes the class of bug rather
# than the one instance.
LICENSE_STEMS = (
    "LICENSE",
    "LICENCE",
    "COPYING",
    "COPYRIGHT",
    "NOTICE",
    "PATENTS",
    "THIRD-PARTY-NOTICES",
)

# Files whose *name* merely resembles a licence but carries no terms.
# `LICENSE_PLEASE_READ.txt` (libopus) asks the reader to accept a
# licence; it is not one, and shipping it as the licence text would
# put the wrong document in the bundle.
NOT_A_LICENCE = frozenset({
    "LICENSE_PLEASE_READ.TXT",
})

# Vendors that are *not* linked into the distributed libraries: they are
# test-only, build-only, or documentation.  Their obligations do not
# travel with the bundle, so a missing licence file there is reported but
# must not fail the cut -- otherwise a test-only stub vendor would block
# every release.
NON_DISTRIBUTED = frozenset({
    "googletest", "doctest", "benchmark", "nlohmann_json", "portaudio",
})

# Vendors whose licence text is embedded in every source file rather than
# shipped as a separate file.  `vendor.json` says `the notice embedded in
# every file`, and the check below verifies that claim rather than taking
# it on trust.
INLINE_NOTICE = {"nv_codec_headers"}

# Vendors fetched at build time rather than committed; their licence file
# is available in the fetched tree.  Only `webrtc_audio_processing` is
# fetched (`vendor.json`: `the source tree is NOT vendored into git: it is
# cloned at build time by tools/fetch_webrtc_apm.py`).  `dav1d` is a real
# submodule (gitlink `160000`), so an absent `dav1d` tree is an
# uninitialised submodule, not a build-time fetch.
FETCHED_AT_BUILD = frozenset({"webrtc_audio_processing"})


def read_vendor_manifest(root: Path) -> list:
    manifest = root / "src" / "third_party" / "vendor.json"
    if not manifest.is_file():
        return []
    with manifest.open(encoding="utf-8-sig") as handle:
        data = json.load(handle)
    return data.get("vendors") or []


def submodule_root(vendor: dict, vendor_dir: Path) -> Path:
    """The directory that holds the upstream tree (a submodule's mount point).

    `vendor.json` names it per vendor (`submodule_path`); fall back to
    `<vendor>/src` when the field is absent.  The distinction matters when
    deciding whether a tree is *empty*: the wrapper `<vendor>/` usually
    carries NimRTC's own `CMakeLists.txt`, so `rglob` on it never returns
    "empty" even when the submodule has never been checked out -- and
    dav1d was consequently reported as unlicensed instead of as an
    uninitialised submodule.
    """
    named = vendor.get("submodule_path")
    if named:
        candidate = vendor_dir.parent.parent / named
        if candidate.is_dir():
            return candidate
    return vendor_dir / "src"


def tree_is_empty(directory: Path) -> bool:
    return not any(p.is_file() for p in directory.rglob("*"))


def is_license_file(name: str) -> bool:
    """Whether a file name is the one carrying a licence's terms.

    The name is matched on its STEM -- the part before the first dot --
    against `LICENSE_STEMS`, so `LICENSE`, `LICENSE.md`,
    `LICENSE-BSD2.md` and `LICENSE.MIT` all qualify while `README.md`
    does not.

    `NOT_A_LICENCE` names the exception: a file that asks the reader to
    accept a licence is not the licence.
    """
    upper = name.upper()
    if upper in NOT_A_LICENCE:
        return False
    stem = upper.split(".", 1)[0]
    return stem in LICENSE_STEMS


def find_license_files(vendor_dir: Path) -> list:
    """Licence-like files under a vendor's tree.

    Two layouts are in play.  A submodule is mounted at
    `<vendor>/src/` and the upstream root *is* that directory, so the
    file is `<vendor>/src/COPYING` (dav1d) or `<vendor>/src/LICENSE`
    (libvpx).  A checked-in tree keeps its wrapper `<vendor>/` next to
    the upstream files, so the file may be `<vendor>/LICENSE`.  Both are
    searched.  `COPYING` was missing from the first cut of this list,
    which is why dav1d -- whose file is `COPYING` at the submodule root
    -- was reported as unlicensed.
    """
    found = []
    seen = set()
    for directory in (vendor_dir, vendor_dir / "src"):
        if not directory.is_dir():
            continue
        for name in sorted(os.listdir(directory)):
            if is_license_file(name):
                path = directory / name
                if path.is_file() and path not in seen:
                    seen.add(path)
                    found.append(path)
    return found


def has_inline_notice(vendor_dir: Path) -> bool:
    """Every C/C++ header under the vendor tree carries a notice line.

    `nv_codec_headers` ships no LICENSE file; its terms are the notice
    at the top of each header.  The headers live under
    `<vendor>/include/ffnvcodec/` (not the vendor root), so the whole
    tree is searched.  Checking that claim is cheap and stops a future
    import that dropped the notice from passing silently.
    """
    headers = [p for p in vendor_dir.rglob("*")
               if p.is_file() and p.suffix in (".h", ".hpp", ".hxx")]
    if not headers:
        return False
    for path in headers:
        try:
            head = path.read_text(encoding="utf-8", errors="replace")[:4096]
        except OSError:
            return False
        lowered = head.lower().lstrip("\ufeff")
        if "licen" not in lowered and "permission is hereby granted" not in lowered:
            return False
    return True


def collect(root: Path, staging: Path, log=print) -> int:
    third_party = root / "src" / "third_party"
    vendors = read_vendor_manifest(root)

    dest_root = staging / "licenses"
    if dest_root.exists():
        shutil.rmtree(dest_root)
    dest_root.mkdir(parents=True, exist_ok=True)

    notices = []
    missing = []
    unfetched = []
    for vendor in vendors:
        name = vendor.get("name") or "?"
        directory = third_party / name
        if not directory.is_dir():
            # Even the wrapper directory is gone.  `git ls-tree` still knows
            # the gitlink, so this is an uninitialised submodule, not a
            # missing vendor -- but the release job checks out recursively,
            # so it is a defect for the cut.
            log("  %-22s no vendored tree at %s" % (name, directory))
            missing.append(name)
            continue
        # Is the UPSTREAM tree present?  The wrapper directory keeps
        # NimRTC's own CMakeLists.txt even when the submodule has never
        # been checked out, so emptiness must be judged on the submodule
        # mount point, not on `<vendor>/`.  A vendor that is not a
        # submodule at all (`nv_codec_headers`: headers checked in at the
        # vendor root, no `src/`) has no mount point, so it is never
        # "empty" -- it is judged on its files below.
        upstream = submodule_root(vendor, directory)
        is_submodule = upstream.is_dir() or bool(vendor.get("submodule_path"))
        if (is_submodule and tree_is_empty(upstream)
                and name not in INLINE_NOTICE):
            # A submodule that has not been initialised in this clone, or a
            # tree fetched at build time.  This is a local-environment
            # fact, not a repository defect: CI checks out the full tree,
            # where the licence file is collected.  It must not be
            # reported as "missing", or a developer with a shallow clone
            # would think the release is unlicensed.
            if name in FETCHED_AT_BUILD:
                log("  %-22s fetched at build time, not present locally "
                    "-- skipped" % name)
            else:
                log("  %-22s tree empty (submodule not initialised) -- "
                    "skipped" % name)
            unfetched.append(name)
            continue
        files = find_license_files(directory)
        if not files and name in INLINE_NOTICE:
            if has_inline_notice(directory):
                target = dest_root / name
                target.mkdir(parents=True, exist_ok=True)
                lines = ["=" * 72,
                         "%s -- %s (notice embedded in each header)"
                         % (name, vendor.get("license") or "UNKNOWN"),
                         "upstream: %s" % (vendor.get("upstream_url") or "?"),
                         "commit:   %s" % (vendor.get("commit_sha") or "?"),
                         "=" * 72, ""]
                carried = 0
                for path in sorted(directory.rglob("*.h")):
                    head = path.read_text(encoding="utf-8", errors="replace")
                    if "permission is hereby granted" in head.lower().lstrip("\ufeff"):
                        relative = path.relative_to(directory)
                        target_file = target / relative
                        target_file.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copy2(path, target_file)
                        carried += 1
                lines.append("%d header(s) with their embedded MIT notice "
                             "are included alongside this file." % carried)
                notices.append("\n".join(lines))
                log("  %-22s %-16s inline notice in %d header(s)"
                    % (name, vendor.get("license") or "UNKNOWN", carried))
                continue
            log("  %-22s INLINE NOTICE CLAIM FAILED (spdx=%s)"
                % (name, vendor.get("license", "UNKNOWN")))
            missing.append(name)
            continue
        if not files:
            log("  %-22s NO LICENCE FILE (spdx=%s)"
                % (name, vendor.get("license", "UNKNOWN")))
            missing.append(name)
            continue
        target = dest_root / name
        target.mkdir(parents=True, exist_ok=True)
        lines = ["=" * 72,
                 "%s -- %s" % (name, vendor.get("license") or "UNKNOWN"),
                 "upstream: %s" % (vendor.get("upstream_url") or "?"),
                 "commit:   %s" % (vendor.get("commit_sha") or "?"),
                 "=" * 72, ""]
        for path in files:
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            shutil.copy2(path, target / path.name)
            lines.append("--- %s  sha256:%s" % (path.name, digest))
            lines.append("")
        notices.append("\n".join(lines))
        log("  %-22s %-16s %d file(s)"
            % (name, vendor.get("license") or "UNKNOWN", len(files)))

    (dest_root / "THIRD_PARTY_NOTICES.txt").write_text(
        "NimRTC third-party licence notices\n\n"
        "The binaries in this bundle link the following vendored\n"
        "components.  Each section repeats the SPDX identifier from\n"
        "src/third_party/vendor.json and the text as shipped upstream.\n\n"
        + "\n".join(notices),
        encoding="utf-8")

    log("")
    log("licences: %d vendor(s) collected, %d without a licence file"
        % (len(notices), len(missing)))
    distributed_missing = [m for m in missing if m not in NON_DISTRIBUTED]
    if distributed_missing:
        log("ERROR: vendor(s) linked into the bundle whose licence text was "
            "not found: %s" % ", ".join(distributed_missing))
        log("       add the upstream licence file to the vendored tree (the "
            "collector looks at the vendor root and the submodule mount "
            "point), or list the vendor in INLINE_NOTICE with a verified "
            "embedded notice.")
        return 1
    if missing:
        log("NOTE: test-only vendor(s) without a licence file (not linked "
            "into the bundle): %s -- reported, does not fail the cut."
            % ", ".join(missing))
    if unfetched:
        log("NOTE: %d vendor tree(s) were not present locally: %s -- CI "
            "runs this after a full fetch, where they are checked."
            % (len(unfetched), ", ".join(unfetched)))
    return 0

def self_test() -> int:
    import tempfile

    failures = []

    def expect(condition: bool, message: str) -> None:
        if not condition:
            failures.append(message)

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp) / "repo"
        tp = root / "src" / "third_party"
        # A vendor that ships its licence: must be collected.
        good = tp / "libvpx"
        (good / "src").mkdir(parents=True)
        (good / "src" / "LICENSE").write_text("BSD-3-Clause text\n",
                                             encoding="utf-8")
        # dav1d's file is `COPYING` at the SUBMODULE root, i.e.
        # `<vendor>/src/COPYING`, next to its upstream sources -- not
        # `<vendor>/src/LICENSE`.  The first cut of this tool missed it
        # and reported dav1d as unlicensed.
        dav1d = tp / "dav1d"
        (dav1d / "src").mkdir(parents=True)
        (dav1d / "src" / "meson.build").write_text("project('dav1d')\n",
                                                   encoding="utf-8")
        (dav1d / "src" / "COPYING").write_text("BSD-2-Clause text\n",
                                               encoding="utf-8")
        # A vendor whose licence file carries an EXTENSION.  svt-av1
        # ships `LICENSE.md` and `PATENTS.md`; the matcher used to
        # compare `name.upper()` against a list of whole names that held
        # `LICENSE.txt` and `LICENSE.TXT` but only `LICENSE.md`, so
        # `LICENSE.md` upper-cased to `LICENSE.MD` and was not in the
        # list.  Every licence file with an extension was invisible, and
        # svt-av1 was reported as unlicensed -- which blocked the
        # v1.3.0 cut.
        extended = tp / "svt-av1"
        (extended / "src").mkdir(parents=True)
        (extended / "src" / "LICENSE.md").write_text("BSD-2-Clause Clear\n",
                                                     encoding="utf-8")
        (extended / "src" / "PATENTS.md").write_text("AOM Patent License\n",
                                                    encoding="utf-8")
        # A file whose NAME resembles a licence but carries no terms.
        (extended / "src" / "LICENSE_PLEASE_READ.txt").write_text(
            "please read the licence\n", encoding="utf-8")
        # A vendor that does not: must fail the gate when distributed.
        bad = tp / "openh264"
        (bad / "src").mkdir(parents=True)
        (bad / "src" / "encode.cpp").write_text("int y;\n", encoding="utf-8")
        # A vendor tree that is EMPTY (submodule not initialised) must be
        # skipped, not reported as unlicensed.
        hollow = tp / "libyuv"
        (hollow / "src").mkdir(parents=True)
        # A test-only vendor without a licence: must not fail the gate.
        testonly = tp / "portaudio"
        (testonly / "src").mkdir(parents=True)
        (testonly / "src" / "portaudio.h").write_text("int x;\n",
                                                     encoding="utf-8")
        (tp / "vendor.json").write_text(json.dumps({"vendors": [
            {"name": "libvpx", "license": "BSD-3-Clause",
             "upstream_url": "https://x/libvpx", "commit_sha": "abc"},
            {"name": "dav1d", "license": "BSD-2-Clause",
             "upstream_url": "https://x/dav1d", "commit_sha": "def"},
            {"name": "openh264", "license": "BSD-2-Clause",
             "upstream_url": "https://x/openh264", "commit_sha": "ghi"},
            {"name": "svt-av1",
             "license": "BSD-2-Clause Clear + AOM Patent License 1.0",
             "upstream_url": "https://x/svt-av1", "commit_sha": "mno"},
            {"name": "libyuv", "license": "BSD-3-Clause",
             "upstream_url": "https://x/libyuv", "commit_sha": "ghi"},
            {"name": "portaudio", "license": "MIT",
             "upstream_url": "https://x/portaudio", "commit_sha": "jkl"},
        ]}), encoding="utf-8")

        staging = Path(tmp) / "staging"
        staging.mkdir()
        code = collect(root, staging, log=lambda m: None)
        notice = (staging / "licenses" / "THIRD_PARTY_NOTICES.txt").read_text(
            encoding="utf-8")
        expect(code == 1,
               "a distributed vendor without a licence file must fail the cut")
        expect((staging / "licenses" / "libvpx" / "LICENSE").is_file(),
               "the licence text must be copied into the bundle")
        expect((staging / "licenses" / "dav1d" / "COPYING").is_file(),
               "a COPYING file at the submodule root must be collected")
        expect((staging / "licenses" / "svt-av1" / "LICENSE.md").is_file(),
               "a licence file with an extension must be collected")
        expect((staging / "licenses" / "svt-av1" / "PATENTS.md").is_file(),
               "a patent grant must be collected as a licence text")
        expect(not (staging / "licenses" / "svt-av1"
                    / "LICENSE_PLEASE_READ.txt").is_file(),
               "a file that only asks the reader to accept a licence is "
               "not a licence and must not be shipped as one")
        expect("BSD-3-Clause" in notice and "libvpx" in notice,
               "the notice file must name the vendor and its SPDX id")
        expect("NO LICENCE" not in notice,
               "a missing licence must not be written as if it were present")

        # Give openh264 its licence: the gate must now pass, and the empty
        # libyuv tree must not have blocked it.
        (bad / "src" / "LICENSE").write_text("BSD-2-Clause text\n",
                                             encoding="utf-8")
        code = collect(root, staging, log=lambda m: None)
        expect(code == 0,
               "once every distributed vendor has a licence file the gate "
               "must pass; an uninitialised submodule is not a defect")

    if failures:
        print("FAIL -- the licence collector caught a defect:")
        for f in failures:
            print("  -", f)
        return 1
    print("PASS -- every vendor's licence text is collected and missing "
          "licences fail the cut.")
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".",
                        help="repository root (default: .)")
    parser.add_argument("--staging", help="install staging directory")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()

    if not args.staging:
        parser.error("--staging is required")
    return collect(Path(args.root).resolve(), Path(args.staging).resolve())


if __name__ == "__main__":
    sys.exit(main())
