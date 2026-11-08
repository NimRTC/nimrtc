#!/usr/bin/env python3
"""Compact status snapshot of the v1.1.0 release gates.

Used by the in-session watcher to answer "where does 1.1 stand?" without
dumping the whole plan document. Every field is derived from a file that
already exists in the repository, so the snapshot cannot drift from the
plan the way a hand-maintained status file would.

Usage:
    python tools/v11_status.py            # human readable
    python tools/v11_status.py --json     # machine readable
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PLAN = REPO / "docs" / "plan" / "v1.1-plan.md"
CHANGELOG = REPO / "CHANGELOG.md"
RELEASING = REPO / "RELEASING.md"
TAG = "v1.1.0"


def _git(*args: str) -> str:
    try:
        out = subprocess.run(
            ["git", "-C", str(REPO), *args],
            capture_output=True,
            text=True,
            timeout=30,
        )
    except (OSError, subprocess.SubprocessError):
        return ""
    return out.stdout.strip()


def _read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def remote_tag() -> str:
    """The sha of the release tag on origin, or "" when it is not there yet."""
    out = _git("ls-remote", "--tags", "origin", f"refs/tags/{TAG}")
    for line in out.splitlines():
        fields = line.split()
        if len(fields) < 2:
            continue
        # Compare the ref name exactly: `ls-remote --tags` prints the
        # annotated tag and its peeled commit as two lines, and only the
        # first one is the tag itself.
        if fields[-1] == f"refs/tags/{TAG}":
            return fields[0]
    return ""


def local_tag() -> str:
    out = _git("rev-list", "-n", "1", TAG)
    return out


def soak() -> dict:
    """Run the soak collector's own summary; it is the gate's source of truth."""
    script = REPO / "tools" / "run_soak.py"
    if not script.exists():
        return {"available": False}
    try:
        proc = subprocess.run(
            [sys.executable, str(script), "--summary", "--json"],
            capture_output=True,
            text=True,
            timeout=120,
            cwd=str(REPO),
        )
    except (OSError, subprocess.SubprocessError):
        return {"available": False}
    try:
        return {"available": True, **json.loads(proc.stdout)}
    except (json.JSONDecodeError, TypeError):
        return {"available": True, "raw": proc.stdout.strip()}


def changelog_section() -> bool:
    return bool(re.search(r"^##\s*\[1\.1\.0\]", _read(CHANGELOG), re.M))


def plan_open_gates() -> list[str]:
    """The unchecked boxes of the release-gate section of the plan.

    A box is "open" when it is not ticked. Lines that carry a struck-out
    marker are counted as closed, because the plan uses that form to record
    a gate that was closed with a note attached.
    """
    text = _read(PLAN)
    if not text:
        return []
    out: list[str] = []
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped.startswith("- ["):
            continue
        if stripped.startswith("- [x]") or stripped.startswith("- [X]"):
            continue
        if re.search(r"~~.*~~", stripped):
            continue
        # keep the leading "[ ]" form out of the label
        label = stripped.lstrip("- [xX ]").strip()
        if label:
            out.append(label)
    return out


def head_commit() -> str:
    return _git("log", "-1", "--format=%h %s")


def collect() -> dict:
    return {
        "tag": TAG,
        "remote_tag_sha": remote_tag(),
        "local_tag_sha": local_tag(),
        "head": head_commit(),
        "changelog_1_1_0_written": changelog_section(),
        "soak": soak(),
        "open_gate_lines": plan_open_gates(),
        "releasing_doc": RELEASING.exists(),
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--json", action="store_true", help="emit JSON")
    args = ap.parse_args()

    data = collect()
    if args.json:
        print(json.dumps(data, indent=2, ensure_ascii=False))
        return 0

    print(f"v1.1.0 status @ {data['head'] or '(no HEAD)'}")
    rt = data["remote_tag_sha"]
    print(f"  remote tag {TAG} : {rt if rt else 'NOT CUT'}")
    print(f"  local  tag {TAG} : {data['local_tag_sha'] or 'absent'}")
    print(
        "  CHANGELOG [1.1.0] : "
        + ("written" if data["changelog_1_1_0_written"] else "NOT written")
    )
    s = data["soak"]
    if s.get("available"):
        print("  soak:")
        for plat, st in (s.get("platforms") or s.get("streaks") or {}).items():
            if isinstance(st, dict):
                print(
                    f"    {plat}: streak {st.get('consecutive_green_days')} "
                    f"(best {st.get('best_streak')})"
                )
    else:
        print("  soak: summary unavailable")
    print(f"  open gate lines in the plan: {len(data['open_gate_lines'])}")
    for line in data["open_gate_lines"]:
        print(f"    - {line[:110]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
