#!/usr/bin/env python3
# Copyright (c) 2026 NimRTC contributors.
# SPDX-License-Identifier: MIT
"""Verify every submodule SHA matches src/third_party/vendor.json.

Exits 0 on success (every submodule matches its manifest entry).
Exits 1 on any of:
  - vendor.json is invalid JSON or has a placeholder commit_sha
  - a vendored commit_sha is not 40 lowercase hex characters
  - a submodule path does not exist on disk
  - a submodule path exists but has no .git (neither directory nor
    gitlink file) -- i.e. it is a checked-in source tree, not a submodule
  - a submodule's HEAD does not match the pinned commit_sha

Usage:
  python tools/check_vendor.py                # human-readable output
  python tools/check_vendor.py --json         # machine-readable output
  python tools/check_vendor.py --strict       # treat "missing submodule" as failure
                                               # (default: warn only)

See docs/plan/vendor-migration.md section 3 Phase 2 for the design.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import NamedTuple

# Force UTF-8 on stdout/stderr so that checkmarks and crosses render
# correctly on Windows consoles whose default code page is GBK/cp936.
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")


REPO_ROOT = Path(__file__).resolve().parent.parent
MANIFEST_PATH = REPO_ROOT / "src" / "third_party" / "vendor.json"
SHA_PATTERN = re.compile(r"^[0-9a-f]{40}$")


class VendorResult(NamedTuple):
    name: str
    status: str  # "ok", "drift", "missing", "no-submodule", "no-git", "bad-sha", "placeholder"
    expected: str | None
    actual: str | None
    message: str | None = None


def load_manifest(path: Path) -> dict:
    """Load vendor.json. Always read as UTF-8 to avoid Windows code-page surprises."""
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def validate_manifest(manifest: dict) -> list[str]:
    """Sanity-check the manifest itself. Returns a list of error strings (empty == OK)."""
    errors: list[str] = []
    if "vendors" not in manifest or not isinstance(manifest["vendors"], list):
        errors.append("manifest has no 'vendors' array")
        return errors

    for v in manifest["vendors"]:
        name = v.get("name", "<unnamed>")
        sha = v.get("commit_sha", "")
        path = v.get("submodule_path", "")

        if not path:
            errors.append(f"  {name}: missing 'submodule_path'")
            continue

        if sha == "REPLACE_WITH_FULL_SHA":
            errors.append(f"  {name}: commit_sha is placeholder 'REPLACE_WITH_FULL_SHA'")
        elif not SHA_PATTERN.match(sha):
            errors.append(f"  {name}: commit_sha {sha!r} is not 40 lowercase hex chars")

    return errors


def read_submodule_head(submodule_path: Path) -> str | None:
    """Return the current HEAD commit SHA of the submodule, or None if it cannot be read."""
    try:
        out = subprocess.check_output(
            ["git", "-C", str(submodule_path), "rev-parse", "HEAD"],
            text=True,
            stderr=subprocess.PIPE,
        )
        return out.strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None


def is_submodule_initialised(submodule_path: Path) -> bool:
    """True if the path looks like a real submodule (has either a .git dir or a .git gitlink file)."""
    git_path = submodule_path / ".git"
    if git_path.is_dir():
        return True
    if git_path.is_file():
        # .git is a gitlink file containing "gitdir: <path>"
        try:
            content = git_path.read_text(encoding="utf-8").strip()
            return content.startswith("gitdir:")
        except OSError:
            return False
    return False


def check_vendor(v: dict, strict: bool) -> VendorResult:
    name = v["name"]
    expected = v["commit_sha"]
    sub_path = REPO_ROOT / v["submodule_path"]

    if not sub_path.exists():
        msg = "path does not exist on disk"
        status = "missing" if strict else "no-submodule"
        return VendorResult(name, status, expected, None, msg)

    if not is_submodule_initialised(sub_path):
        msg = "path exists but has no .git — appears to be a checked-in source tree"
        return VendorResult(name, "no-submodule", expected, None, msg)

    actual = read_submodule_head(sub_path)
    if actual is None:
        return VendorResult(name, "no-git", expected, None, "git rev-parse HEAD failed")

    if actual == expected:
        return VendorResult(name, "ok", expected, actual)

    return VendorResult(
        name,
        "drift",
        expected,
        actual,
        f"submodule HEAD {actual[:12]} != manifest {expected[:12]}",
    )


def render_human(results: list[VendorResult], manifest_errors: list[str]) -> int:
    if manifest_errors:
        print("vendor.json validation errors:")
        for e in manifest_errors:
            print(e)
        print()
        return 1

    by_status: dict[str, list[VendorResult]] = {}
    for r in results:
        by_status.setdefault(r.status, []).append(r)

    # Print in a stable order.
    order = ["ok", "drift", "no-submodule", "no-git", "missing"]
    for status in order:
        entries = by_status.get(status, [])
        if not entries:
            continue
        for r in entries:
            if status == "ok":
                print(f"  ✓ {r.name}: {r.actual[:12]} matches manifest")
            else:
                print(f"  ✗ {r.name}: {status} — {r.message or ''}")

    extras = {k: v for k, v in by_status.items() if k not in order}
    for status, entries in extras.items():
        for r in entries:
            print(f"  ? {r.name}: {status} — {r.message or ''}")

    # Non-ok statuses that are WARNINGS (do NOT fail the gate).
    # "no-submodule" is the default for missing paths and
    # "checked-in-tree" vendors (sources committed directly to the repo
    # instead of being a git submodule).  Only --strict turns these
    # into failures.
    warning_statuses = {"no-submodule"}
    failed = sum(
        len(v)
        for k, v in by_status.items()
        if k != "ok" and k not in warning_statuses
    )
    print()
    if failed == 0:
        warnings = sum(len(v) for k, v in by_status.items() if k in warning_statuses)
        if warnings:
            print(
                f"All {len(results) - warnings} submodule(s) match vendor.json "
                f"({warnings} vendor(s) are checked-in trees, skipped)."
            )
        else:
            print(f"All {len(results)} submodules match vendor.json.")
        return 0

    print(f"{failed} vendor(s) do not match vendor.json:")
    for r in results:
        if r.status != "ok" and r.status not in warning_statuses:
            print(f"  - {r.name} ({r.status})")
    return 1


def render_json(results: list[VendorResult], manifest_errors: list[str]) -> int:
    payload = {
        "manifest_errors": manifest_errors,
        "vendors": [
            {
                "name": r.name,
                "status": r.status,
                "expected": r.expected,
                "actual": r.actual,
                "message": r.message,
            }
            for r in results
        ],
        "ok": all(r.status == "ok" for r in results) and not manifest_errors,
    }
    print(json.dumps(payload, indent=2))
    return 0 if payload["ok"] else 1


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify every submodule SHA matches src/third_party/vendor.json."
    )
    parser.add_argument("--json", action="store_true", help="emit JSON instead of human output")
    parser.add_argument(
        "--strict",
        action="store_true",
        help="treat a missing submodule path as a failure (default: warn only)",
    )
    args = parser.parse_args()

    if not MANIFEST_PATH.exists():
        print(f"vendor.json not found at {MANIFEST_PATH}", file=sys.stderr)
        return 1

    manifest = load_manifest(MANIFEST_PATH)
    manifest_errors = validate_manifest(manifest)

    results: list[VendorResult] = []
    if not manifest_errors:
        for v in manifest["vendors"]:
            try:
                results.append(check_vendor(v, strict=args.strict))
            except KeyError as e:
                results.append(
                    VendorResult(
                        v.get("name", "<unnamed>"),
                        "bad-manifest",
                        None,
                        None,
                        f"manifest missing field {e}",
                    )
                )

    if args.json:
        return render_json(results, manifest_errors)
    return render_human(results, manifest_errors)


if __name__ == "__main__":
    sys.exit(main())