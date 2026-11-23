#!/usr/bin/env python3
"""Re-cut the v1.3.0 tag once the soak window closes green.

Why this exists
---------------
`scripts/recut_v130_tag.py` already knows how to re-cut the tag: it runs
the soak gate, the freeze checklist and the signature check, and it
refuses to touch the tag unless all of them pass.  What it does not do
is *wait* -- the window needs consecutive green days on both
platforms, and on 2026-11-22 Linux was short (a Chrome-interop
timeout on 2026-11-20 broke its streak).

So this watcher is the loop around it: poll, and when the gate passes,
run the re-cut for real.  It does not re-implement any of the gate --
`recut_v130_tag.py` remains the only thing that decides whether the tag
moves, and this script never moves the tag itself.

What it does
------------
Every `--interval-minutes` (default 60):

  1. run `scripts/recut_v130_tag.py --check-only` -- the authoritative
     gate, in the repository's own tree, with the evidence fetched the
     way the release fetches it;
  2. if the gate passes, run the same script WITHOUT `--check-only`,
     which re-cuts, signs and force-pushes the tag, and then exit;
  3. otherwise sleep, and keep the reason for the report.

The gate is the only thing consulted.  This watcher has no opinion about
whether a window "looks good enough" -- if the gate says no, the answer
is no, however long that takes.

Usage:
    python scripts/watch_release_recut.py --repo D:\\MyOpen\\NimRTC
    python scripts/watch_release_recut.py --once     # a single poll
"""

import argparse
import os
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path


def log(message):
    """Print a timestamped line, tolerating a GBK console.

    The console on a Chinese-locale Windows is GBK, and the gate's output
    carries characters GBK cannot encode.  A `print` would raise
    UnicodeEncodeError and take the watcher down; a character the console
    cannot show becomes '?' instead.  The verdict is carried by the exit
    code, never by the text.
    """
    try:
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError):
        pass
    stamp = datetime.now().astimezone().strftime("%Y-%m-%d %H:%M:%S")
    print("[%s] %s" % (stamp, message), flush=True)


def run(cmd, cwd, timeout=1800):
    """Run a command, returning (rc, output).  Never raises on failure."""
    try:
        proc = subprocess.run(
            cmd, cwd=str(cwd), capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired:
        return 1, "the command timed out after %s s" % timeout
    except OSError as exc:
        return 1, "the command could not be started: %s" % exc
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


def repo_root(repo):
    repo = Path(repo).resolve()
    if not (repo / "scripts" / "recut_v130_tag.py").is_file():
        raise SystemExit("FATAL: %s is not the NimRTC repository "
                         "(scripts/recut_v130_tag.py is missing)" % repo)
    return repo


def main():
    ap = argparse.ArgumentParser(
        description="Poll the soak gate and re-cut v1.3.0 once it "
                    "closes green on both platforms.")
    ap.add_argument("--repo", default=os.environ.get("NIMRTC_REPO", "."),
                    help="the repository to work in (default: the cwd)")
    ap.add_argument("--interval-minutes", type=float, default=60.0,
                    help="how long to wait between polls (default: 60)")
    ap.add_argument("--once", action="store_true",
                    help="poll once and exit, instead of looping")
    ap.add_argument("--max-polls", type=int, default=0,
                    help="stop after this many polls (0 = forever)")
    args = ap.parse_args()

    repo = repo_root(args.repo)
    recut = repo / "scripts" / "recut_v130_tag.py"
    python = sys.executable

    log("watching %s" % repo)
    log("the gate is scripts/recut_v130_tag.py; this watcher never moves "
        "the tag itself")

    polls = 0
    while True:
        polls += 1
        log("poll %d: running the gate (--check-only)" % polls)

        rc, out = run([python, str(recut), "--check-only"], cwd=repo)

        if rc == 0:
            log("the gate PASSED -- re-cutting the tag for real")
            rc2, out2 = run([python, str(recut)], cwd=repo)
            log(out2.strip()[-2000:])
            if rc2 == 0:
                log("=== the tag was re-cut and pushed ===")
                return 0
            # The re-cut may have created the local tag before it
            # failed, so "the tag was not moved" would be a claim this
            # watcher cannot make.  What IS true is that this watcher
            # did not push anything: origin still carries whatever it
            # carried before.
            log("the re-cut failed (rc=%d); nothing was pushed, so origin "
                "still carries the old tag -- the script above says why"
                % rc2)
            return rc2

        # The gate said no.  Keep the reason: it is the whole point of
        # the wait, and it is what a human asks for when they ask "why
        # is the release still not out?".
        reason = ""
        for line in out.splitlines():
            if "window NOT satisfied" in line or "GATE FAILED" in line:
                reason = line.strip()
                break
        log("the gate says no: %s" % (reason or "exit %d" % rc))
        log("the tag stays where it is; waiting %.0f min" %
            args.interval_minutes)

        if args.once:
            return rc
        if args.max_polls and polls >= args.max_polls:
            log("reached --max-polls=%d; stopping" % args.max_polls)
            return rc

        time.sleep(args.interval_minutes * 60.0)


if __name__ == "__main__":
    sys.exit(main())
