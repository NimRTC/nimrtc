#!/usr/bin/env python3
# Copyright (c) 2026 NimRTC contributors.
# SPDX-License-Identifier: MIT
"""Apply the NimRTC patch series to the vendored WebRTC APM source tree.

The WebRTC Audio Processing Module (APM) is NOT vendored into git — it is
cloned at build time by ``tools/fetch_webrtc_apm.py`` into
``src/third_party/webrtc_audio_processing/src/`` and built out-of-tree with
Meson (see ``tools/build_webrtc_apm.{cmd,sh}``).

Because the upstream tree is fetched rather than committed, any change
NimRTC needs in it has to travel as a versioned patch rather than as an
edit inside the clone.  This script is the single place where that happens.
It applies the patches in
``src/third_party/webrtc_audio_processing/patches/*.patch`` in lexical
order and is idempotent: a patch that is already applied is skipped, so
running it repeatedly is safe.

Design notes:

* Patches are applied with ``git apply`` inside the APM clone.  That clone
  is a real git repository, so ``git apply`` works without needing the
  ``patch(1)`` utility, and it validates strictly — a patch that does not
  apply cleanly is a hard error rather than a silent mis-merge.
* Each patch is applied in its own ``git apply`` invocation so that a
  failure names the offending patch.
* "Already applied" is detected with ``git apply --reverse --check``: if
  the patch can be reverse-applied cleanly, it is already in the tree.

Usage:
    python tools/apply_webrtc_apm_patches.py
    python tools/apply_webrtc_apm_patches.py --check
    python tools/apply_webrtc_apm_patches.py --source-dir <path>

Exit codes:
    0  all patches applied (or already present)
    1  a patch failed to apply
    2  the APM source tree is missing
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")


REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SOURCE_DIR = REPO_ROOT / "src" / "third_party" / "webrtc_audio_processing" / "src"
DEFAULT_PATCH_DIR = REPO_ROOT / "src" / "third_party" / "webrtc_audio_processing" / "patches"


def git_apply_check(source_dir: Path, patch_path: Path, reverse: bool) -> tuple[bool, str]:
    """Run ``git apply --check`` (optionally ``--reverse``) in source_dir.

    Returns (ok, combined_output).
    """
    cmd = ["git", "apply"]
    if reverse:
        cmd.append("--reverse")
    cmd.append("--check")
    cmd.append(str(patch_path))

    result = subprocess.run(
        cmd,
        cwd=str(source_dir),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    return result.returncode == 0, result.stdout or ""


def git_apply_real(source_dir: Path, patch_path: Path) -> tuple[bool, str]:
    """Actually apply the patch (not a dry run)."""
    cmd = ["git", "apply", str(patch_path)]
    result = subprocess.run(
        cmd,
        cwd=str(source_dir),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    return result.returncode == 0, result.stdout or ""


def is_already_applied(source_dir: Path, patch_path: Path) -> bool:
    """True when the patch is already present in the tree."""
    ok, _ = git_apply_check(source_dir, patch_path, reverse=True)
    return ok


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Apply the NimRTC WebRTC APM patch series to the APM source tree."
    )
    parser.add_argument(
        "--source-dir",
        type=Path,
        default=DEFAULT_SOURCE_DIR,
        help="APM source tree to patch (default: src/third_party/webrtc_audio_processing/src)",
    )
    parser.add_argument(
        "--patch-dir",
        type=Path,
        default=DEFAULT_PATCH_DIR,
        help="directory holding *.patch files (default: src/third_party/webrtc_audio_processing/patches)",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="dry run: report which patches would apply, change nothing",
    )
    args = parser.parse_args()

    source_dir: Path = args.source_dir
    patch_dir: Path = args.patch_dir

    if not source_dir.is_dir():
        print(
            f"[ERROR] APM source tree not found at {source_dir}\n"
            f"        Run 'python tools/fetch_webrtc_apm.py' first.",
            file=sys.stderr,
        )
        return 2

    if not (source_dir / "meson.build").exists():
        print(
            f"[ERROR] {source_dir} does not look like an APM checkout "
            f"(meson.build is missing).",
            file=sys.stderr,
        )
        return 2

    if not patch_dir.is_dir():
        # No patches is a perfectly valid state.
        print(f"[INFO] No patch directory at {patch_dir}; nothing to apply.")
        return 0

    patches = sorted(patch_dir.glob("*.patch"))
    if not patches:
        print(f"[INFO] No patches in {patch_dir}; nothing to apply.")
        return 0

    print(f"[INFO] APM source : {source_dir}")
    print(f"[INFO] Patch dir  : {patch_dir}")
    print(f"[INFO] Patches    : {len(patches)}")
    print()

    failed = False
    for patch_path in patches:
        name = patch_path.name

        if is_already_applied(source_dir, patch_path):
            print(f"[SKIP] {name} (already applied)")
            continue

        if args.check:
            # Verify it *would* apply cleanly, without touching the tree.
            ok, output = git_apply_check(source_dir, patch_path, reverse=False)
            if ok:
                print(f"[OK  ] {name} (would apply)")
            else:
                failed = True
                print(f"[FAIL] {name} (would not apply)")
                if output.strip():
                    print(output.rstrip())
            continue

        ok, output = git_apply_real(source_dir, patch_path)
        if ok:
            print(f"[OK  ] {name} (applied)")
        else:
            failed = True
            print(f"[FAIL] {name}")
            if output.strip():
                print(output.rstrip())
            print(
                "        The upstream APM tree has likely drifted from the\n"
                "        pinned commit. Refresh the clone and re-apply:\n"
                "          rm -rf <source-dir> && python tools/fetch_webrtc_apm.py"
            )

    print()
    if failed:
        print("[ERROR] One or more patches failed to apply.")
        return 1

    if args.check:
        print("[OK] All patches are in the expected state (dry run).")
    else:
        print("[OK] WebRTC APM patches applied.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
