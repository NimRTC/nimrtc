#!/usr/bin/env python3
# Copyright (c) 2026 NimRTC contributors.
# SPDX-License-Identifier: MIT
"""Report whether a release bundle ships the WebRTC APM archives.

Why this is a script and not a shell one-liner
----------------------------------------------

The obvious spelling is

    tar -tJf "$bundle" 2>/dev/null | grep -q "webrtc_apm/"

and it is **wrong** under `set -o pipefail`, which every bash step in
`.github/workflows/release.yml` sets.  `grep -q` exits as soon as it
finds its first match, which closes the read end of the pipe; `tar` is
still writing the member list and receives SIGPIPE, so it exits 141.
With `pipefail` the pipeline's status is then 141 (the rightmost
non-zero status wins), the `if` sees a failure, and the step concludes
"this bundle ships no APM archives".

That is not a hypothetical.  The published `v1.1.0` release body says
`lib/webrtc_apm/ is absent` for all four platforms, while the
`linux-x86_64` and `linux-aarch64` bundles *do* carry
`lib/webrtc_apm/libwebrtc-audio-processing-2.a` and 14 more members.
The table was wrong because of the pipeline, not because of the
packages.

This script lists the bundle with Python's `tarfile` instead, so there
is no pipeline, no SIGPIPE, and no dependence on the runner's `tar`
being able to read xz.

Exit codes
----------

    0   the bundle lists at least one member under `lib/webrtc_apm/`
    1   the bundle was read successfully and carries no APM archives
    2   the bundle could not be read (missing, corrupt, not a tarball)

`1` and `2` are distinct on purpose: the release body says "no APM
archives" for the first and "could not be checked" for the second, and
the two must not be conflated.

Usage:
    python tools/check_bundle_apm.py <bundle.tar.xz>
    python tools/check_bundle_apm.py <bundle.tar.xz> --json
    python tools/check_bundle_apm.py --self-test
"""

from __future__ import annotations

import argparse
import io
import json
import sys
import tarfile
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")


# The directory the release body talks about.  A member counts when it
# is *inside* that directory -- the directory entry itself and the
# archives under it both qualify, and the check is a prefix test on the
# normalised member name.
APM_DIR = "lib/webrtc_apm/"

# The member names are `./lib/webrtc_apm/...` because the bundle is
# packed with `tar -C <staging> .`.  Normalise the leading `./` away so
# the prefix test does not depend on how the bundle was packed.
def normalise(name: str) -> str:
    """Strip the leading `./` a `tar -C dir .` packing leaves on every member."""
    while name.startswith("./"):
        name = name[2:]
    return name


def apm_members(bundle: Path) -> list[str]:
    """Return the bundle members that live under `lib/webrtc_apm/`.

    Raises `OSError` when the bundle cannot be read.
    """
    with tarfile.open(bundle, "r:*") as tf:
        return [
            m.name
            for m in tf.getmembers()
            if normalise(m.name).startswith(APM_DIR)
        ]


def classify(bundle: Path) -> tuple[int, list[str], str]:
    """Return `(exit_code, apm_members, message)` for one bundle."""
    if not bundle.is_file():
        return 2, [], f"{bundle}: no such bundle"
    try:
        members = apm_members(bundle)
    except (tarfile.TarError, OSError, EOFError) as exc:
        return 2, [], f"{bundle}: cannot read the bundle ({exc})"
    if not members:
        return 1, [], f"{bundle}: no APM archives in the bundle"
    return 0, members, f"{bundle}: {len(members)} APM archive member(s) in the bundle"


# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------


def _strip_yaml_comments(text: str) -> str:
    """Remove YAML comments, keeping the line structure intact.

    A `#` only starts a comment when it is at the start of the line or
    preceded by whitespace -- inside a quoted scalar it is ordinary
    text.  The workflow's bash steps are block scalars (`run: |`), and
    a `#` inside them *is* a shell comment, so those lines are dropped
    too; that is exactly what we want, because the pattern we look for
    must not survive in a comment.
    """
    out: list[str] = []
    for line in text.splitlines():
        stripped = line.lstrip()
        if stripped.startswith("#"):
            out.append("")
            continue
        # A `#` preceded by whitespace starts a comment; one glued to a
        # word (e.g. `sha256:#`) is part of the scalar.
        idx = 0
        cut = len(line)
        while True:
            pos = line.find("#", idx)
            if pos == -1:
                break
            if pos == 0 or line[pos - 1] in " \t":
                cut = pos
                break
            idx = pos + 1
        out.append(line[:cut])
    return "\n".join(out)


def _write_tar(path: Path, members: list[tuple[str, bytes]]) -> None:
    """Write a tar.xz with the given `(member name, payload)` pairs."""
    with tarfile.open(path, "w:xz") as tf:
        for name, payload in members:
            info = tarfile.TarInfo(name)
            info.size = len(payload)
            tf.addfile(info, io.BytesIO(payload))


def self_test() -> int:
    """Pin the behaviour that matters, including the SIGPIPE regression.

    The regression is not reproduced through a shell pipeline here -- it
    is asserted structurally: `classify` must report a bundle that
    *does* carry the archives as carrying them, and the shell one-liner
    must not be used anywhere in the release workflow.  The second half
    is checked against the workflow file itself, because a "fix" that
    reintroduces `tar ... | grep -q` would pass every functional test
    here and still ship the wrong table.
    """
    import tempfile

    failures: list[str] = []

    def check(condition: bool, what: str) -> None:
        if not condition:
            failures.append(what)

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)

        # 1. A bundle that carries the APM archives must be recognised.
        with_apm = root / "with-apm.tar.xz"
        _write_tar(
            with_apm,
            [
                ("./lib/", b""),
                ("./lib/webrtc_apm/", b""),
                ("./lib/webrtc_apm/libwebrtc-audio-processing-2.a", b"\x00" * 16),
                ("./lib/libnimrtc_engine.a", b"\x00" * 16),
            ],
        )
        code, members, _ = classify(with_apm)
        check(code == 0, "a bundle with lib/webrtc_apm/ must exit 0")
        check(len(members) == 2, f"expected 2 APM members, got {len(members)}")

        # 2. A bundle without them must exit 1 -- "read fine, no APM".
        without_apm = root / "without-apm.tar.xz"
        _write_tar(
            without_apm,
            [
                ("./lib/", b""),
                ("./lib/libnimrtc_engine.a", b"\x00" * 16),
                ("./lib/webrtc_apm_not/", b""),
            ],
        )
        code, members, _ = classify(without_apm)
        check(code == 1, "a bundle without lib/webrtc_apm/ must exit 1")
        check(members == [], f"expected no APM members, got {members}")

        # 3. The near-miss directory `lib/webrtc_apm_not/` must not count.
        check(
            not any("webrtc_apm_not" in m for m in members),
            "lib/webrtc_apm_not/ must not be counted as the APM directory",
        )

        # 4. A missing bundle is "cannot be checked" (2), not "no APM" (1).
        code, _, _ = classify(root / "does-not-exist.tar.xz")
        check(code == 2, "a missing bundle must exit 2")

        # 5. A file that is not a tarball is also "cannot be checked".
        garbage = root / "garbage.tar.xz"
        garbage.write_bytes(b"this is not a tarball")
        code, _, _ = classify(garbage)
        check(code == 2, "a non-tarball must exit 2")

    # 6. The release workflow must not contain the pipeline that broke it.
    #    The check looks for the pattern *in code*, not in prose: the
    #    comments explaining the bug legitimately name it, and a naive
    #    substring test would flag the very commit that fixes it.
    workflow = Path(__file__).resolve().parent.parent / ".github" / "workflows" / "release.yml"
    if workflow.is_file():
        code = _strip_yaml_comments(workflow.read_text(encoding="utf-8", errors="replace"))
        check(
            "tar -tJf" not in code,
            "release.yml still uses `tar -tJf`: it must call this script instead "
            "(`tar | grep -q` under `pipefail` reports every bundle as APM-free)",
        )
        check(
            "check_bundle_apm.py" in code,
            "release.yml must call `tools/check_bundle_apm.py` to decide the 3A table",
        )
    else:
        failures.append(f"release.yml not found at {workflow}")

    if failures:
        print("self-test FAILED:")
        for f in failures:
            print(f"  - {f}")
        return 1

    print("self-test OK: the APM-presence check distinguishes present / absent / unreadable")
    return 0


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("bundle", nargs="?", type=Path, help="the .tar.xz bundle to inspect")
    parser.add_argument("--json", action="store_true", help="emit a machine-readable verdict")
    parser.add_argument("--self-test", action="store_true", help="run the built-in self-test")
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()

    if args.bundle is None:
        parser.error("a bundle path is required (or use --self-test)")

    code, members, message = classify(args.bundle)
    if args.json:
        print(
            json.dumps(
                {
                    "bundle": str(args.bundle),
                    "has_apm": code == 0,
                    "apm_member_count": len(members),
                    "apm_members": members,
                    "readable": code != 2,
                    "message": message,
                },
                indent=2,
            )
        )
    else:
        print(message)
    return code


if __name__ == "__main__":
    sys.exit(main())
