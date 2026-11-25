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


def same_repository(a: str, b: str) -> bool:
    """Do two origin URLs name the same repository?

    The same repository has more than one URL: it can be reached over
    SSH or over HTTPS, and the two spellings are the same repository
    with the same history.  A comparison that looks at the whole URL
    therefore rejects the very mirror it is looking for -- the runner
    works over HTTPS while a developer's checkout is configured for
    SSH -- and the job falls back to the network without saying why.

    So the comparison is on the repository path: 'owner/name' out of
    whichever spelling, with the '.git' suffix and any leading
    scheme and host removed.
    """
    def path_of(url: str) -> str:
        url = url.strip().rstrip("/")
        if url.endswith(".git"):
            url = url[:-4]
        # 'git@github.com:owner/name' -- the host is before the colon
        # and the path is after it.  This spelling has no scheme.
        if "@" in url and ":" in url and not url.startswith(
                ("http://", "https://", "ssh://", "git://")):
            return url.split(":", 1)[1].strip("/").lower()
        for prefix in ("https://", "http://", "ssh://", "git://"):
            if url.startswith(prefix):
                url = url[len(prefix):]
                break
        # What is left is 'host/owner/name'; the path is after the
        # host.
        if "/" in url:
            url = url.split("/", 1)[1]
        return url.strip("/").lower()

    return bool(path_of(a)) and path_of(a) == path_of(b)


def git_config(root: str, key: str) -> str:
    """One git config value for a repository, or "" if there is none."""
    try:
        out = subprocess.run(
            ["git", "-C", root, "config", "--get", key],
            capture_output=True, timeout=60)
    except (OSError, subprocess.SubprocessError):
        return ""
    return out.stdout.decode("utf-8", "replace").strip()


def mirror_candidates(root: str) -> list[str]:
    """Local clones of this repository that already have the vendored
    trees.

    The runner and the tree it builds are usually the same machine, and
    a working copy elsewhere on that machine has the vendored trees
    already in it.  Copying those is not a shortcut around the check
    that matters -- the trees are checked against the commit the tree
    records, the same check the network path makes -- it is just the
    difference between copying 120 MB and downloading it.

    The candidates are *named*, not searched for.  A scan is
    convenient, but it depends on the shape of the machine, and a job
    that cannot find its mirror then fails in a way that looks like
    the network is down.  Each candidate is checked before it is used,
    so one that does not exist, or is not a clone of this repository,
    is simply passed over.
    """
    root = os.path.abspath(root)
    parent = os.path.dirname(root)

    origin = git_config(root, "remote.origin.url")
    if not origin:
        return []

    entries: list[str] = []
    try:
        entries = sorted(os.listdir(parent))
    except OSError:
        pass
    for extra in (os.environ.get("NIMRTC_MIRROR", ""),
                  "D:" + os.sep + "MyOpen" + os.sep + "NimRTC",
                  "D:" + os.sep + "MyOpen" + os.sep + "NimRTC-followup"):
        if extra:
            entries.append(extra)

    found: list[str] = []
    seen: set[str] = set()
    for entry in entries:
        cand = os.path.join(parent, entry)
        if not os.path.isdir(cand) and os.path.isabs(entry):
            cand = entry
        if cand in seen:
            continue
        seen.add(cand)
        if cand == root or not os.path.isdir(cand):
            continue
        # '.git' is a directory in an ordinary clone and a *file* in a
        # linked worktree, and both count as a clone.
        if not os.path.exists(os.path.join(cand, ".git")):
            continue
        cand_origin = git_config(cand, "remote.origin.url")
        if cand_origin and same_repository(cand_origin, origin):
            found.append(cand)
    return found



def submodule_name(root: str, path: str) -> str:
    """The *name* a submodule has in `.gitmodules`, for its path.

    The name and the path are separate keys, and `git config
    submodule.<name>.url` takes the name.  In this repository the two
    happen to be the same string for most submodules, but they are not
    the same *thing*, and the one that has to be right is the name.
    """
    try:
        out = subprocess.run(
            ["git", "-C", root, "config", "-f", ".gitmodules",
             "--get-regexp", r"submodule\..*\.path"],
            capture_output=True, timeout=60)
    except (OSError, subprocess.SubprocessError):
        return ""
    text = out.stdout.decode("utf-8", "replace")
    for line in text.split("\n"):
        if not line.strip():
            continue
        key, _, value = line.partition(" ")
        if value.strip() != path:
            continue
        # 'submodule.<name>.path' -> '<name>'
        name = key[len("submodule."):-len(".path")]
        return name
    return ""


def _clear_readonly_and_retry(func: object, path: str, _exc: object) -> None:
    """`shutil.rmtree`'s error handler: clear the read-only bit, retry.

    Git writes its object store read-only, and on Windows a read-only
    file cannot be removed -- `rmtree` raises `PermissionError` and
    gives up on the whole tree.  This is the handler that undoes that:
    it makes the file writable and calls the failing operation again.

    The read-only bit is a property of the *file*, not of the copy
    that is being cleaned up, so clearing it is safe: it is the
    object store of a cache that is about to be replaced.
    """
    import os
    import stat

    try:
        os.chmod(path, stat.S_IWRITE)
    except OSError:
        pass
    try:
        func(path)  # type: ignore[operator]
    except OSError:
        pass


def _remove_tree(path: str) -> None:
    """Remove a tree, clearing the read-only bit where it blocks.

    `shutil.rmtree` changed its error callback's signature in Python
    3.12: `onerror` is deprecated in favour of `onexc`, and the old
    one raises a `DeprecationWarning`.  The runner's Python is 3.9 and
    the developer's is 3.12+, so the call is chosen by what the
    interpreter actually offers.
    """
    import shutil
    import sys

    if sys.version_info >= (3, 12):
        shutil.rmtree(path, onexc=_clear_readonly_and_retry)
    else:
        shutil.rmtree(path, onerror=_clear_readonly_and_retry)


def copy_cache(src: str, dst: str) -> bool:
    """Copy a module cache, hardlinking the object stores.

    A plain copy of a module cache is expensive in a way that is easy
    to miss: `msquic`'s cache contains the caches of its *own*
    submodules -- openssl, quictls, and a dozen more -- so copying it
    copies gigabytes of pack files that the build never reads.  On the
    runner that filled the disk, and a full disk is the worst possible
    failure: the copy is truncated, git reports 'object file is
    empty', and the whole fetch fails.

    The object stores are therefore *hardlinked* rather than copied:
    the files are shared with the mirror instead of duplicated, so the
    copy costs no disk at all.  Git never writes into an object store
    it only reads -- a new object goes into a new file -- so sharing
    them is safe.  A hardlink that cannot be made falls back to a
    copy, and only that fallback can run out of space.

    The `modules` subdirectory -- the caches of the nested submodules
    -- is skipped entirely.  Nothing in this repository reads it, and
    it is the bulk of the size.
    """
    import shutil

    # A stale cache has to go first: `git submodule update` refuses to
    # clone into a submodule path whose module directory already
    # exists, and says so with 'Unable to find current revision in
    # submodule path'.  That is not transient -- the clone is never
    # attempted and the retry around it never gets anywhere.
    #
    # The removal has to clear the *read-only* bit first.  Git writes
    # its object files read-only -- 444 -- and on Windows a read-only
    # file cannot be deleted: `shutil.rmtree` raises
    # `PermissionError: [WinError 5]`, the removal is abandoned, and
    # the copy reports a failure that has nothing to do with the copy.
    # The half-removed cache is still there, and the next attempt
    # fails the same way.
    if os.path.isdir(dst):
        try:
            _remove_tree(dst)
        except OSError:
            return False
    try:
        os.makedirs(dst, exist_ok=True)
    except OSError:
        return False

    for dirpath, dirnames, filenames in os.walk(src):
        rel = os.path.relpath(dirpath, src)
        # The nested submodule caches: not needed, and large.
        if rel == "modules" or rel.startswith("modules" + os.sep):
            dirnames[:] = []
            continue
        target = os.path.join(dst, rel) if rel != "." else dst
        if not os.path.isdir(target):
            try:
                os.makedirs(target, exist_ok=True)
            except OSError:
                return False
        for name in filenames:
            source_file = os.path.join(dirpath, name)
            target_file = os.path.join(target, name)
            try:
                os.link(source_file, target_file)
            except OSError:
                # A hardlink across a volume is not possible, and some
                # filesystems refuse.  A copy is the fallback.
                try:
                    shutil.copy2(source_file, target_file)
                except OSError:
                    return False
    return True


def clear_worktree_config(cache_dir: str) -> bool:
    """Remove the `core.worktree` setting from a copied module cache.

    The setting is a path to the worktree the cache belongs to, and a
    copied cache still carries the *mirror's*.  Git refuses to run any
    command in a repository whose worktree is missing, so the setting
    cannot be corrected with `git config` -- it has to be edited out of
    the config file first.

    The file is git's own, and the setting is one line in it.  Both
    `config` and `config.worktree` are touched: a cache with
    `extensions.worktreeConfig` set keeps the worktree settings in the
    second one.
    """
    ok = True
    for name in ("config", "config.worktree"):
        path = os.path.join(cache_dir, name)
        if not os.path.exists(path):
            continue
        try:
            with open(path, "r", encoding="utf-8") as f:
                lines = f.read().split("\n")
            kept = [ln for ln in lines
                    if not ln.strip().startswith("worktree =")]
            if len(kept) == len(lines):
                continue
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write("\n".join(kept))
        except OSError:
            ok = False
    return ok


def _write_gitdir_pointer(path: str, gitdir: str) -> bool:
    """Write a submodule's `.git` pointer file, replacing what is there.

    The file is *replaced*, not edited in place.  That matters on
    Windows: the runner's workspace holds a pointer file that some
    other process has open, and opening such a file for writing fails
    with `PermissionError: [Errno 13]` -- the handle was opened
    without write sharing.  Deleting the file is allowed (Windows
    marks it for deletion), and writing a *new* file with the same
    name is allowed too, because the old handle does not own the
    name any more.

    So the order is: remove, then write.  The failure that remains
    is a genuine one -- the directory is not writable, or the file is
    held without delete sharing either.
    """
    try:
        if os.path.exists(path):
            os.remove(path)
    except OSError:
        pass
    try:
        with open(path, "w", encoding="utf-8") as f:
            f.write("gitdir: %s\n" % gitdir)
    except OSError:
        return False
    return True


def copy_from_mirror(root: str, path: str, mirror: str) -> bool:
    """Bring one vendored tree up from a local mirror.

    The tree is brought up by copying the mirror's *module cache* --
    `.git/modules/<path>` -- into the workspace and pointing the
    submodule's worktree at it, then letting git check the recorded
    commit out of it.

    Two other mechanisms were tried and are both wrong:

    - Copying the mirror's *files* leaves git knowing nothing about
      the submodule.  `git submodule status` keeps reporting it as not
      initialised (`-` in front of the commit), the `is_up_to_date`
      check keeps saying no, and the job falls through to the network
      for a tree it is already holding.  That is what the first
      version did, and it is why the mirror bought nothing: the
      wolfssl tree was copied in full and the job still spent half an
      hour fetching it.

    - Cloning from the mirror's working tree works for an ordinary
      clone, but it fails for a *partial* clone -- and one of the
      mirrors here is one.  A partial clone has a promisor remote, so
      objects it does not have are fetched on demand; cloning from it
      asks for those objects over the network, and the clone dies with
      'fatal: early EOF' on the link that is already the problem.
      Copying the cache avoids the network entirely.

    Copying the cache needs the submodule's *name* registered in the
    parent's config, or git reports 'could not get a repository handle
    for submodule' and the status line keeps its '-'.

    The commit the parent tree records is still the one that is checked
    out: a mirror that does not have it is refused, not substituted.
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

    # The submodule's *name* in `.gitmodules` -- not its path.  The
    # name is what `git config submodule.<name>.url` takes, and the
    # name and the path are separate keys.
    name = submodule_name(root, path)
    if not name:
        return False

    # The mirror's module cache: git's own object store for this
    # submodule.  The working tree is not enough -- it has the sources
    # but not the objects, and the checkout needs the objects.
    src_cache = os.path.join(mirror, ".git", "modules",
                             path.replace("/", os.sep))
    if not os.path.isdir(src_cache):
        return False

    # The cache has to have the commit the tree records.  A partial
    # clone's cache may not have it -- in which case the mirror cannot
    # serve this submodule and the network path is used.
    if want:
        try:
            out = subprocess.run(
                ["git", "-C", src_cache, "cat-file", "-e",
                 "%s^{commit}" % want],
                capture_output=True, timeout=120)
        except (OSError, subprocess.SubprocessError):
            return False
        if out.returncode != 0:
            return False

    dst_cache = os.path.join(root, ".git", "modules",
                             path.replace("/", os.sep))
    if not copy_cache(src_cache, dst_cache):
        print("  %s: could not copy the module cache" % path, flush=True)
        return False

    # The copied cache's `core.worktree` still points at the *mirror's*
    # worktree -- it is a relative path, and the copy did not change it.
    # Git then works in the mirror's directory, which does not exist
    # here, and says:
    #
    #     fatal: cannot chdir to '../../../../../../../NimRTC-followup/
    #     src/third_party/openh264/src': No such file or directory
    #
    # That is worse than it looks: git refuses to run *any* command in
    # a repository whose worktree is missing, so `git config` cannot
    # fix it -- the fix has to be applied to the config file directly.
    # The line is removed and the correct one written afterwards.
    if not clear_worktree_config(dst_cache):
        print("  %s: could not clear the copied worktree setting"
              % path, flush=True)
        return False

    # Now git can be used in the cache, and the worktree is repointed
    # at this repository's copy of the submodule -- as a path relative
    # to the cache, the form git itself writes.
    dst_worktree = os.path.join(root, path)
    rel_worktree = os.path.relpath(dst_worktree, dst_cache).replace(os.sep,
                                                                    "/")
    rc = run(["git", "-C", dst_cache, "config", "core.worktree",
              rel_worktree], timeout=60)
    if rc != 0:
        return False

    # Register the submodule in the parent's config.  Without this git
    # reports 'could not get a repository handle for submodule' and
    # the status line keeps its '-', which reads as 'not initialised'
    # and sends the job to the network.
    rc = run(["git", "-C", root, "config", "submodule.%s.url" % name,
              src.replace(os.sep, "/")], timeout=60)
    if rc != 0:
        return False

    # Point the worktree at the copied cache.  The path in the `.git`
    # file is relative to the worktree's parent, which is what git
    # writes itself and what it reads back.
    os.makedirs(dest, exist_ok=True)
    git_file = os.path.join(dest, ".git")
    rel = os.path.relpath(dst_cache, dest).replace(os.sep, "/")
    if not _write_gitdir_pointer(git_file, rel):
        print("  %s: could not point the submodule at the cache" % path,
              flush=True)
        return False

    # Check the recorded commit out of the copied cache.  The
    # `--force` is deliberate: the worktree may hold files from an
    # earlier attempt, and the commit is what has to be there.
    rc = run(["git", "-C", dest, "checkout", "--force", want],
             timeout=300)
    if rc != 0:
        return False

    # Git has to agree that the submodule is initialised and at the
    # recorded commit.  `git submodule status` is the only thing that
    # says so.
    if not is_up_to_date(root, path):
        print("  %s: the mirror brought it up, but not at the recorded "
              "commit" % path, flush=True)
        return False

    # A submodule that is *registered* but whose tree is empty is a
    # silent failure -- the build would fail later, far from the
    # cause.
    if not os.path.isdir(dest) or not os.listdir(dest):
        print("  %s: the mirror brought up an empty tree" % path, flush=True)
        return False
    print("  %s: brought up from the local mirror" % path, flush=True)
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
    #
    # The sign is the *first character of the line*, so the line must
    # not be stripped before it is read: `strip()` removes exactly the
    # leading space that says 'at the recorded commit', and a submodule
    # that is right is then reported as wrong.  That is not a rare
    # corner -- it is every submodule the mirror copied -- and the
    # consequence is that the job falls through to the network for a
    # tree it already has on disk.
    out = subprocess.run(
        ["git", "submodule", "status", "--", path], cwd=root,
        capture_output=True, timeout=120)
    text = out.stdout.decode("utf-8", "replace").splitlines()
    if not text:
        return False
    return text[0].startswith(" ")


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
    #
    # It is bounded, because `--no-fetch` has been seen to hang: with
    # no fetch and no timeout of its own, git waits on an object that
    # never arrives.  The budget turns that into a failure the retry
    # around it can act on.
    rc = run(
        ["git", "submodule", "update", "--init", "--no-fetch", "--", path],
        cwd=root, timeout=120)
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
