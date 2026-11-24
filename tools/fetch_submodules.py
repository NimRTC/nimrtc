#!/usr/bin/env python3
"""Fetch the tree, retrying the transfers that get cut off.

`actions/checkout` clones the repository and, with `submodules:
recursive`, every submodule too -- all in one pass, with no retry.  On
a link that drops long transfers that is the wrong shape: the largest
vendored tree (`wolfssl`) is over 100 MB, and a transfer cut halfway
leaves git with `early EOF` and nothing to resume from.  The same is
true of the repository's own fetch, which the checkout also does
without a retry.

So this does the fetching itself, one thing at a time, with a bounded
retry around each.  A transfer that dies is retried; the retry is
cheap because a partial pack is not kept, but the *repository* is, so
what actually gets re-fetched is the missing part.

    python tools/fetch_submodules.py --retries 5              # submodules
    python tools/fetch_submodules.py --fetch-main --retries 5 # the repo

`--fetch-main` re-runs the checkout's own fetch, for the case where
the checkout step failed on a cut-off transfer: it fetches the
requested ref into the already-initialised repository, so it does not
have to clone from scratch.
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


# The git settings that make a transfer survive a flaky link.  They are
# passed per-invocation rather than read from a config file, so the
# behaviour does not depend on the caller's environment.
GIT_TUNING = [
    "-c", "http.version=HTTP/1.1",
    "-c", "http.postBuffer=524288000",
    # A low-speed *floor* rather than none: with no floor at all a
    # transfer that has stalled outright waits forever, and the retry
    # around it never gets a chance to fire.  This floor is low enough
    # that a merely slow transfer survives, and high enough that a dead
    # one is abandoned in minutes and retried.
    "-c", "http.lowSpeedLimit=1024",
    "-c", "http.lowSpeedTime=120",
]


def fetch_one(root: str, path: str, retries: int) -> bool:
    """Fetch one submodule, retrying a cut-off transfer."""
    for attempt in range(1, retries + 1):
        # `git submodule update --init` is idempotent: a submodule that is
        # already at the right commit is a no-op, so a retry after a
        # partial failure resumes rather than starting over.
        rc = run(
            ["git"] + GIT_TUNING
            + ["submodule", "update", "--init", "--depth", "1", "--", path],
            cwd=root)
        if rc == 0:
            return True
        print("  %s: attempt %d/%d failed (rc=%d)"
              % (path, attempt, retries, rc), flush=True)
        if attempt < retries:
            time.sleep(RETRY_BACKOFF_SECONDS)
    return False


def fetch_main(root: str, ref: str, retries: int) -> bool:
    """Fetch the repository's own ref, retrying a cut-off transfer.

    The repository is already initialised by the checkout, so this
    fetches into it rather than cloning: the objects that did arrive
    are kept, and only the missing ones are asked for again.
    """
    for attempt in range(1, retries + 1):
        rc = run(
            ["git"] + GIT_TUNING
            + ["fetch", "--no-tags", "--prune", "--depth=1",
               "origin", ref],
            cwd=root)
        if rc == 0:
            return True
        print("  origin %s: attempt %d/%d failed (rc=%d)"
              % (ref, attempt, retries, rc), flush=True)
        if attempt < retries:
            time.sleep(RETRY_BACKOFF_SECONDS)
    return False


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", default=".", help="the repository root")
    ap.add_argument("--retries", type=int, default=DEFAULT_RETRIES)
    ap.add_argument("--fetch-main", action="store_true",
                    help="also (re-)fetch the repository's own ref")
    ap.add_argument("--ref", default="",
                    help="the ref to fetch with --fetch-main")
    args = ap.parse_args()

    root = os.path.abspath(args.root)

    if args.fetch_main:
        ref = args.ref or os.environ.get("GITHUB_SHA", "")
        if not ref:
            sys.exit("--fetch-main needs --ref or GITHUB_SHA")
        print("fetching origin %s (up to %d attempts)"
              % (ref, args.retries), flush=True)
        if not fetch_main(root, ref, args.retries):
            print("FAILED: could not fetch origin %s" % ref, flush=True)
            return 1
        print("fetched origin %s" % ref, flush=True)

    paths = submodule_paths(root)
    if not paths:
        print("no submodules declared -- nothing to fetch")
        return 0

    print("fetching %d submodule(s) with up to %d attempts each"
          % (len(paths), args.retries), flush=True)

    failed: list[str] = []
    for path in paths:
        print("- %s" % path, flush=True)
        if not fetch_one(root, path, args.retries):
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
