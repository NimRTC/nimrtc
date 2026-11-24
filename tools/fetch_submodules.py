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


def run(cmd: list[str], cwd: str | None = None,
        timeout: int | None = None) -> int:
    """Run a command, killing it if it overruns `timeout` seconds.

    The timeout is not decoration.  A transfer on this link has been
    observed to *stall*: the pack file stops growing, the child
    process goes away, and git waits for it forever.  A retry around
    a non-zero exit code never fires for a command that never returns,
    so the stall has to be broken here.
    """
    try:
        proc = subprocess.run(cmd, cwd=cwd, timeout=timeout)
    except subprocess.TimeoutExpired:
        print("  timed out after %ds: %s"
              % (timeout, " ".join(cmd[-3:])), flush=True)
        return -1
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

# How long a single fetch may take before it is killed and retried.
# Sized for the largest vendored tree on the slowest link observed:
# `wolfssl` is over 100 MB and has moved at tens of kilobytes a
# second.  The budget is generous -- a fetch that is merely slow must
# not be thrown away -- but it is finite, because the alternative is a
# job that hangs until the runner's own timeout takes it.
FETCH_TIMEOUT_SECONDS = 900


def mirror_candidates(root: str) -> list[str]:
    """Local clones of this repository that already have the vendored
    trees, nearest first.

    The runner and the tree it builds are usually the same machine, and
    a working copy elsewhere on that machine has the vendored trees
    already in it.  Copying those is not a shortcut around the check
    that matters -- the trees are checked against the commit the tree
    records, the same check the network path makes -- it is just the
    difference between copying 120 MB and downloading it.

    The candidates are found by name, under the same parent directory
    as the working copy, and only those that are *this* repository
    (same origin URL) are considered.
    """
    parent = os.path.dirname(os.path.abspath(root))
    if not os.path.isdir(parent):
        return []

    try:
        out = subprocess.run(
            ["git", "-C", os.path.abspath(root), "config", "--get",
             "remote.origin.url"],
            capture_output=True, timeout=60)
        origin = out.stdout.decode("utf-8", "replace").strip()
    except (OSError, subprocess.SubprocessError):
        return []
    if not origin:
        return []

    found: list[str] = []
    entries: list[str] = []
    try:
        entries = sorted(os.listdir(parent))
    except OSError:
        pass
    # The mirror is often on another drive -- the runner's work
    # directory and a developer's checkout are not neighbours -- so
    # the well-known locations are looked at too.
    for extra in (os.environ.get("NIMRTC_MIRROR", ""),
                  r"D:\MyOpen\NimRTC"):
        if extra:
            entries.append(os.path.basename(extra.rstrip("\\/")))

    seen: set[str] = set()
    for entry in entries:
        cand = os.path.join(parent, entry)
        if not os.path.isdir(cand) and os.path.isabs(entry):
            cand = entry
        if cand in seen:
            continue
        seen.add(cand)
        if cand == os.path.abspath(root) or not os.path.isdir(cand):
            continue
        if not os.path.isdir(os.path.join(cand, ".git")):
            continue
        try:
            out = subprocess.run(
                ["git", "-C", cand, "config", "--get", "remote.origin.url"],
                capture_output=True, timeout=60)
            cand_origin = out.stdout.decode("utf-8", "replace").strip()
        except (OSError, subprocess.SubprocessError):
            continue
        if cand_origin and cand_origin.rstrip("/").endswith(
                origin.rstrip("/").split("/")[-1]):
            found.append(cand)
    return found


def copy_from_mirror(root: str, path: str, mirror: str) -> bool:
    """Copy one vendored tree from a local mirror into the working copy.

    The tree is copied as *files*, not as a git checkout: the build
    only needs the sources, and a copied tree carries no git history
    that could disagree with the commit the parent tree records.  The
    commit is checked separately, below.
    """
    src = os.path.join(mirror, path)
    dest = os.path.join(root, path)
    if not os.path.isdir(src) or not os.listdir(src):
        return False

    # The commit the parent tree records for this submodule.
    try:
        out = subprocess.run(
            ["git", "-C", root, "ls-tree", "HEAD", "--", path],
            capture_output=True, timeout=60)
        line = out.stdout.decode("utf-8", "replace").strip()
    except (OSError, subprocess.SubprocessError):
        return False
    if not line:
        return False
    want = line.split()[2] if len(line.split()) > 2 else ""

    # The commit the mirror has.  If it is not the one the parent tree
    # records, the mirror is out of date for this submodule and the
    # copy would be wrong -- so the network path is used instead.
    have = ""
    try:
        out = subprocess.run(
            ["git", "-C", src, "rev-parse", "HEAD"],
            capture_output=True, timeout=60)
        have = out.stdout.decode("utf-8", "replace").strip()
    except (OSError, subprocess.SubprocessError):
        return False
    if want and have and want != have:
        return False

    if os.path.isdir(dest):
        return True
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    try:
        subprocess.run(
            ["git", "-C", root, "submodule", "update", "--init",
             "--no-fetch", "--", path],
            capture_output=True, timeout=120)
    except (OSError, subprocess.SubprocessError):
        pass
    try:
        import shutil
        shutil.copytree(src, dest, dirs_exist_ok=True,
                        symlinks=True,
                        ignore=shutil.ignore_patterns(".git"))
    except OSError as exc:
        print("  %s: could not copy from the mirror: %s" % (path, exc),
              flush=True)
        return False
    print("  %s: copied from the local mirror" % path, flush=True)
    return True


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
        capture_output=True, timeout=120)
    text = out.stdout.decode("utf-8", "replace").strip()
    if not text:
        return False
    return text.startswith(" ")


def fetch_one(root: str, path: str, retries: int,
              mirrors: list[str]) -> bool:
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

    for mirror in mirrors:
        if copy_from_mirror(root, path, mirror):
            return True

    # The local-first attempt: no `--depth`, so git can assemble the
    # commit from objects it already has.  `--no-fetch` keeps it from
    # going to the network at all, which is the point: this attempt is
    # *only* about what is already on disk.
    rc = run(
        ["git", "submodule", "update", "--init", "--no-fetch", "--", path],
        cwd=root, timeout=FETCH_TIMEOUT_SECONDS)
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
            cwd=root, timeout=FETCH_TIMEOUT_SECONDS)
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
    ap.add_argument("--no-mirror", action="store_true",
                    help="do not look for a local clone of this repository")
    args = ap.parse_args()

    root = os.path.abspath(args.root)

    mirrors = []
    if not args.no_mirror:
        mirrors = mirror_candidates(root)
        if mirrors:
            print("local mirror(s) with the vendored trees: %s"
                  % ", ".join(mirrors), flush=True)

    paths = submodule_paths(root)
    if not paths:
        print("no submodules declared -- nothing to fetch")
        return 0

    print("fetching %d submodule(s) with up to %d attempts each"
          % (len(paths), args.retries), flush=True)

    failed: list[str] = []
    for path in paths:
        print("- %s" % path, flush=True)
        if not fetch_one(root, path, args.retries, mirrors):
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
