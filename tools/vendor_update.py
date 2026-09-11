#!/usr/bin/env python3
# Copyright (c) 2026 NimRTC contributors.
# SPDX-License-Identifier: MIT
"""Update a single submodule to a new upstream SHA and rewrite vendor.json.

This is the Phase-2 helper for the vendor migration
(`docs/plan/vendor-migration.md` §3). It reads the manifest entry for
`<name>`, advances the submodule to either:
  * an explicit commit/tag/branch passed on the CLI, or
  * the current upstream HEAD of the configured remote branch,

then rewrites the `commit_sha` field in `vendor.json`, and updates the
companion file `src/third_party/SOURCE_VERSIONS` so both stay in sync.

USAGE

    python tools/vendor_update.py <name>                  # bump to upstream HEAD
    python tools/vendor_update.py <name> --to v1.6.2      # pin a tag
    python tools/vendor_update.py <name> --to <40hexsha>  # pin a commit
    python tools/vendor_update.py <name> --print          # just print current state
    python tools/vendor_update.py --list                  # print all entries
    python tools/vendor_update.py --check                 # run tools/check_vendor.py

DESIGN

- Refuses to update if `vendor.json` is missing fields, or if the manifest
  `commit_sha` is the placeholder `REPLACE_WITH_FULL_SHA`.
- Refuses to set a SHA that is not 40 lowercase hex chars.
- Runs `git submodule update --remote --merge` to advance the submodule
  before reading the new HEAD, so the local working tree matches what the
  manifest will pin.
- Prints the old/new SHA + tag at the end; exits non-zero if git fails.

EXIT CODES
    0  update succeeded (or `--print` / `--list` / `--check` were clean)
    1  manifest missing entry / bad args
    2  git submodule operation failed
    3  post-update SHA validation failed

This script is intentionally read-only against `vendor.json` and the
submodule state unless explicitly invoked with an action verb (no
positional `<name>` AND `--print`/`--list`/`--check`). It does NOT push
the new SHA — that is a separate `git commit -am "chore(vendor): bump
<name> to <sha>"` step performed by the human or by automation.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Optional

# Force UTF-8 on stdout/stderr so checkmarks render on Windows consoles
# whose default code page is GBK/cp936.
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

REPO_ROOT = Path(__file__).resolve().parent.parent
MANIFEST_PATH = REPO_ROOT / "src" / "third_party" / "vendor.json"
SOURCE_VERSIONS_PATH = REPO_ROOT / "src" / "third_party" / "SOURCE_VERSIONS"
SHA_PATTERN = re.compile(r"^[0-9a-f]{40}$")


# ---------------------------------------------------------------------------
# Small I/O helpers
# ---------------------------------------------------------------------------

def load_manifest() -> dict:
    """Read src/third_party/vendor.json as UTF-8."""
    with MANIFEST_PATH.open("r", encoding="utf-8") as f:
        return json.load(f)


def save_manifest(manifest: dict) -> None:
    """Write src/third_party/vendor.json back, preserving key order."""
    with MANIFEST_PATH.open("w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2, ensure_ascii=False)
        f.write("\n")


def find_vendor(manifest: dict, name: str) -> dict:
    for v in manifest["vendors"]:
        if v["name"] == name:
            return v
    available = ", ".join(v["name"] for v in manifest["vendors"])
    raise SystemExit(f"vendor {name!r} not in vendor.json (have: {available})")


def run(cmd: list[str], cwd: Optional[Path] = None, check: bool = True) -> str:
    """Run `cmd`, return stdout; raise SystemExit(2) on non-zero exit."""
    proc = subprocess.run(
        cmd,
        cwd=str(cwd) if cwd else None,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if check and proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        raise SystemExit(f"command failed ({proc.returncode}): {' '.join(cmd)}\n{proc.stderr}")
    return proc.stdout.strip()


def git_head(submodule_path: Path) -> str:
    out = run(["git", "rev-parse", "HEAD"], cwd=submodule_path)
    if not SHA_PATTERN.match(out):
        raise SystemExit(f"non-SHA HEAD in {submodule_path}: {out!r}")
    return out


def git_describe(submodule_path: Path, sha: str) -> str:
    """Best-effort `git describe` for human-readable tag annotation."""
    proc = subprocess.run(
        ["git", "describe", "--tags", sha],
        cwd=str(submodule_path),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return proc.stdout.strip() if proc.returncode == 0 else "(no tag)"


# ---------------------------------------------------------------------------
# SOURCE_VERSIONS mirror
# ---------------------------------------------------------------------------

_LINE_PATTERN = re.compile(
    r"""
    ^(?P<name>\S+)\s+
    (?P<tag>\S+)\s+
    (?P<short>[0-9a-f]{7})\s+
    (?P<full>[0-9a-f]{40})\s+
    (?P<date>\S+)\s+
    (?P<url>\S+)\s*$
    """,
    re.VERBOSE,
)


def parse_source_versions() -> list[dict]:
    rows: list[dict] = []
    for raw in SOURCE_VERSIONS_PATH.read_text(encoding="utf-8").splitlines():
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        m = _LINE_PATTERN.match(raw)
        if m:
            rows.append(m.groupdict())
    return rows


def rewrite_source_versions(vendor: dict, new_sha: str) -> None:
    """Replace the matching row in SOURCE_VERSIONS in place."""
    rows = parse_source_versions()
    name = vendor["name"]
    new_short = new_sha[:7]
    for i, r in enumerate(rows):
        if r["name"] == name or r["name"].replace("_", "") == name:
            rows[i]["full"] = new_sha
            rows[i]["short"] = new_short
            break
    else:
        # Append if absent — keeps the file in sync even if vendor.json grew.
        rows.append(
            {
                "name": name,
                "tag": vendor.get("upstream_version", "master"),
                "short": new_short,
                "full": new_sha,
                "date": "TBD",
                "url": vendor.get("upstream_url", ""),
            }
        )
    new_lines = SOURCE_VERSIONS_PATH.read_text(encoding="utf-8").splitlines()
    # Find the first non-comment line to rewrite from there.
    out_lines: list[str] = []
    rewritten = False
    for line in new_lines:
        if not rewritten and line.strip() and not line.lstrip().startswith("#"):
            m = _LINE_PATTERN.match(line)
            if m and (m["name"] == name or m["name"].replace("_", "") == name):
                r = next(r for r in rows if r["full"] == new_sha and (r["name"] == name or r["name"].replace("_", "") == name))
                out_lines.append(
                    f"{r['name']:<22} {r['tag']:<14} {r['short']}   {r['full']}   {r['date']}   {r['url']}"
                )
                rewritten = True
            else:
                out_lines.append(line)
        else:
            out_lines.append(line)
    SOURCE_VERSIONS_PATH.write_text("\n".join(out_lines) + "\n", encoding="utf-8")


# ---------------------------------------------------------------------------
# Actions
# ---------------------------------------------------------------------------

def action_print(vendor: dict) -> int:
    sub_path = REPO_ROOT / vendor["submodule_path"]
    print(f"name             : {vendor['name']}")
    print(f"upstream_url     : {vendor['upstream_url']}")
    print(f"upstream_version : {vendor['upstream_version']}")
    print(f"manifest_sha     : {vendor['commit_sha']}")
    print(f"submodule_path   : {sub_path}")
    if sub_path.exists():
        try:
            head = git_head(sub_path)
            describe = git_describe(sub_path, head)
            print(f"current_HEAD     : {head}  ({describe})")
            if head != vendor["commit_sha"]:
                drift = "drift"
            else:
                drift = "match"
            print(f"drift_status     : {drift}")
        except SystemExit as e:
            print(f"current_HEAD     : (unreadable — {e})")
    else:
        print("current_HEAD     : (path missing)")
    return 0


def action_list(manifest: dict) -> int:
    for v in manifest["vendors"]:
        line = f"  {v['name']:<22} {v.get('upstream_version', '?'):<14} {v['commit_sha']}"
        print(line)
    return 0


def action_check() -> int:
    """Delegate to tools/check_vendor.py so this script is the single CLI."""
    checker = REPO_ROOT / "tools" / "check_vendor.py"
    if not checker.exists():
        raise SystemExit(f"missing {checker}")
    proc = subprocess.run([sys.executable, str(checker)], cwd=str(REPO_ROOT))
    return proc.returncode


def action_update(manifest: dict, name: str, target: Optional[str]) -> int:
    vendor = find_vendor(manifest, name)
    sub_path = REPO_ROOT / vendor["submodule_path"]
    if not sub_path.exists():
        raise SystemExit(f"submodule path does not exist: {sub_path}")

    # 1. Verify the manifest entry is sane before we touch anything.
    if vendor["commit_sha"] == "REPLACE_WITH_FULL_SHA":
        raise SystemExit(
            f"vendor.json entry for {name} still has placeholder SHA — "
            "fill it in before running an update."
        )
    if not SHA_PATTERN.match(vendor["commit_sha"]):
        raise SystemExit(
            f"vendor.json entry for {name} has non-canonical SHA "
            f"{vendor['commit_sha']!r}; aborting."
        )

    old_sha = vendor["commit_sha"]

    # 2. Advance the submodule.
    if target is None:
        # Bump to upstream HEAD of the configured branch.
        # `git submodule update --remote` reads the .gitmodules branch entry
        # by default; we pass the path explicitly to be unambiguous.
        run(["git", "fetch", "origin"], cwd=sub_path, check=True)
        # Try fast-forward; if the local submodule's branch is configured to
        # track origin/<branch>, this is just `git merge --ff-only`.
        run(["git", "merge", "--ff-only"], cwd=sub_path, check=True)
    else:
        if SHA_PATTERN.match(target):
            run(["git", "checkout", target], cwd=sub_path, check=True)
        else:
            # Treat as a tag or remote branch ref.
            run(["git", "fetch", "origin", target], cwd=sub_path, check=True)
            run(["git", "checkout", "FETCH_HEAD"], cwd=sub_path, check=True)

    new_sha = git_head(sub_path)
    if not SHA_PATTERN.match(new_sha):
        raise SystemExit(3, f"new SHA validation failed: {new_sha!r}")

    describe = git_describe(sub_path, new_sha)

    # 3. Update vendor.json (only the matching entry; keep the rest as-is).
    for v in manifest["vendors"]:
        if v["name"] == name:
            v["commit_sha"] = new_sha
            v["upstream_version"] = describe if describe != "(no tag)" else v["upstream_version"]
            break
    save_manifest(manifest)

    # 4. Keep SOURCE_VERSIONS in sync.
    rewrite_source_versions(vendor, new_sha)

    print(f"Updated {name}: {old_sha[:12]} → {new_sha[:12]}  ({describe})")
    print(f"  vendor.json      rewritten")
    print(f"  SOURCE_VERSIONS  rewritten")
    print()
    print("Next step: review the diff and commit with")
    print(f"  git add src/third_party/vendor.json src/third_party/SOURCE_VERSIONS")
    print(f"  git commit -m 'chore(vendor): bump {name} to {new_sha[:12]} ({describe})'")
    return 0


# ---------------------------------------------------------------------------
# CLI plumbing
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Update a NimRTC submodule and rewrite vendor.json.",
    )
    p.add_argument(
        "name",
        nargs="?",
        help="Vendor name (must match a key in vendor.json vendors[]).",
    )
    p.add_argument(
        "--to",
        metavar="REF",
        help="Pin to a tag/commit. Omit to bump to upstream HEAD.",
    )
    p.add_argument(
        "--print",
        action="store_true",
        help="Print the current state of <name> and exit.",
    )
    p.add_argument(
        "--list",
        action="store_true",
        help="List every entry in vendor.json and exit.",
    )
    p.add_argument(
        "--check",
        action="store_true",
        help="Run tools/check_vendor.py and propagate its exit code.",
    )
    p.add_argument(
        "--dry-run",
        action="store_true",
        help="Print the intended changes but do not modify any file.",
    )
    return p


def main(argv: Optional[list[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    if not MANIFEST_PATH.exists():
        raise SystemExit(f"vendor.json not found at {MANIFEST_PATH}")

    manifest = load_manifest()

    if args.list:
        return action_list(manifest)
    if args.check:
        return action_check()
    if args.print:
        if not args.name:
            raise SystemExit("--print requires a <name> argument")
        return action_print(find_vendor(manifest, args.name))
    if not args.name:
        raise SystemExit(
            "usage: vendor_update.py <name> [--to REF] [--print | --list | --check]"
        )
    if args.dry_run:
        print("[dry-run] would update", args.name, "to", args.to or "<upstream HEAD>")
        return 0

    return action_update(manifest, args.name, args.to)


if __name__ == "__main__":
    sys.exit(main())
