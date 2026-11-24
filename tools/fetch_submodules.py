#!/usr/bin/env python3
"""Fetch the vendored submodules, retrying the ones that get cut off.

`actions/checkout` with `submodules: recursive` clones every submodule in
one pass, and a single failure aborts the step.  On a link that drops
long transfers, the vendored trees are exactly the wrong shape for that:
the largest of them (`wolfssl`) is over 100 MB, and a transfer that is
cut halfway leaves git with `early EOF` and nothing to resume from.

So this does the fetching itself, one submodule at a time, with a
bounded retry around each.  A transfer that dies is retried; the retry
is cheap because a partial pack is not kept, but the *repository* is, so
what actually gets re-fetched is the missing part.

    python tools/fetch_submodules.py --retries 5
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time

DEFAULT_RETRIES = 5
RETRY_BACKOFF_SECONDS = 5


def run(cmd: list[str], cwd: str | None = None) -> int:
    proc = subprocess.run(cmd, cwd=cwd)
    return proc.returncode


def submodule_paths(root: str) -> list[str]:
    """The submodule paths, in `.gitmodules` order."""
    gitmodules = os.path.join(root, ".gitmodules")
    if not os.path.exists(gitmodules):
        return []
    paths: list[str] = []
    with open(gitmodules, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line.startswith("path"):
                parts = line.split("=", 1)
                if len(parts) == 2:
                    paths.append(parts[1].strip())
    return paths


def fetch_one(root: str, path: str, url: str, retries: int) -> bool:
    """Fetch one submodule, retrying a cut-off transfer."""
    dest = os.path.join(root, path)
    for attempt in range(1, retries + 1):
        # `git submodule update --init` is idempotent: a submodule that is
        # already at the right commit is a no-op, so a retry after a
        # partial failure resumes rather than starting over.
        rc = run([
            "git", "-c", "http.version=HTTP/1.1",
            "-c", "http.postBuffer=524288000",
            "-c", "http.lowSpeedLimit=0",
            "-c", "http.lowSpeedTime=999999",
            "submodule", "update", "--init", "--depth", "1", "--", path,
        ], cwd=root)
        if rc == 0:
            return True
        print("  %s: attempt %d/%d failed (rc=%d)"
              % (path, attempt, retries, rc), flush=True)
        if attempt < retries:
            time.sleep(RETRY_BACKOFF_SECONDS)
    return False


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", default=".", help="the repository root")
    ap.add_argument("--retries", type=int, default=DEFAULT_RETRIES)
    args = ap.parse_args()

    root = os.path.abspath(args.root)
    paths = submodule_paths(root)
    if not paths:
        print("no submodules declared -- nothing to fetch")
        return 0

    print("fetching %d submodule(s) with up to %d attempts each"
          % (len(paths), args.retries), flush=True)

    failed: list[str] = []
    for path in paths:
        print("- %s" % path, flush=True)
        if not fetch_one(root, path, "", args.retries):
            failed.append(path)

    if failed:
        print("FAILED: %s" % ", ".join(failed), flush=True)
        return 1

    # A submodule that is *registered* but whose tree is empty is a
    # silent failure -- the build would fail later, far from the cause.
    empty = []
    for path in paths:
        dest = os.path.join(root, path)
        if not os.path.isdir(dest) or not os.listdir(dest):
            empty.append(path)
    if empty:
        print("FAILED: these submodules are empty: %s" % ", ".join(empty),
              flush=True)
        return 1

    print("all %d submodule(s) fetched" % len(paths), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
