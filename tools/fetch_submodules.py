#!/usr/bin/env python3
"""Fetch the vendored submodules, retrying the ones that get cut off.

`actions/checkout` with `submodules: recursive` clones every submodule
in one pass, and a single failure aborts the step.  On a link that
drops long transfers, the vendored trees are exactly the wrong shape
for that: the largest of them (`wolfssl`) is over 100 MB, and a
transfer that is cut halfway leaves git with `early EOF` and nothing
to resume from.

So this does the fetching itself, one submodule at a time, with a
bounded retry around each.  A transfer that dies is retried; the
retry is cheap because a partial pack is not kept, but the
*repository* is, so what actually gets re-fetched is the missing part.

    python tools/fetch_submodules.py --retries 5

Two things make it faster on a runner that has been here before.  The
first is the local object cache: '.git/modules' survives from one job
to the next, so a submodule whose objects are already on disk is
checked out without a transfer.  The second is that a submodule
already sitting at the commit the tree records is skipped outright.

The repository's own fetch is not here: `hardware.yml` does that in
the preceding step, with the same bounded retry, because the checkout
action cannot retry its own transfer.
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


def is_up_to_date(root: str, path: str) -> bool:
    """Is this submodule already checked out at the commit the tree wants?

    `git submodule update --init` is a no-op for a submodule that is
    already right, so the check only saves the *stat* -- but it also
    answers the question the update answers, and on a runner that has
    been here before it saves the transfer too.
    """
    rc = run(["git", "submodule", "status", "--", path], cwd=root)
    if rc != 0:
        return False
    # `git submodule status` prefixes the commit with a sign: a leading
    # '-' means the submodule is not initialised, '+' means it is at a
    # different commit than the tree records, and ' ' means it is at the
    # recorded commit.
    out = subprocess.run(
        ["git", "submodule", "status", "--", path], cwd=root,
        capture_output=True)
    text = out.stdout.decode("utf-8", "replace").strip()
    if not text:
        return False
    return text.startswith(" ")


def fetch_one(root: str, path: str, retries: int) -> bool:
    """Fetch one submodule, retrying a cut-off transfer.

    The order of attempts matters here.  The runner's `.git/modules`
    directory survives from one job to the next -- `actions/checkout`
    cleans the *worktree*, not the object cache -- so on a runner that
    has been here before, the objects for most of the vendored trees
    are already on disk.  Asking git to fetch them again is pure
    latency on a link that is not fast, and on this one it is the
    difference between a job that finishes and a job that times out.

    So the local attempt comes first, without `--depth`: it lets git
    use the objects it already has and go to the network only for the
    ones it is missing.  The shallow fetch is the fallback, for the
    submodule whose objects are not in the cache yet.
    """
    if is_up_to_date(root, path):
        print("  %s: already at the recorded commit" % path, flush=True)
        return True

    # The local-first attempt: no `--depth`, so git can assemble the
    # commit from objects it already has.  `--no-fetch` keeps it from
    # going to the network at all, which is the point: this attempt is
    # *only* about what is already on disk.
    rc = run(
        ["git", "submodule", "update", "--init", "--no-fetch", "--", path],
        cwd=root)
    if rc == 0 and is_up_to_date(root, path):
        print("  %s: checked out from the local object cache" % path,
              flush=True)
        return True

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
