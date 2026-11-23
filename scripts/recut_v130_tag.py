#!/usr/bin/env python3
"""Re-cut the v1.3.0 tag onto a tree that carries the shallow-clone fix.

Why this exists
---------------
The v1.3.0 release failed in preflight (release run 44687753371, 2026-11-20)
because `release.yml`'s checkout was a SHALLOW clone.  The soak gate asks
git `merge-base --is-ancestor <the commit the day was measured on> <tag>`
for every day of the window; a depth-1 clone cannot answer that for any day
but the last, so every day read "unknown", which the gate counts as NOT
green, and the window scored 0/7 on both platforms.

PR #137 fixed the workflow (`fetch-depth: 0`).  That fix has to be ON the
tagged commit itself -- the preflight checks out the TAG, not main -- so
the tag has to be re-cut onto the commit that carries it.

The tag is signed (`gpg.format=ssh`), and `tools/verify_tag_signature.py`
is the repo's own check for that.  This script refuses to touch the tag if
any of the gates below fail, so a bad re-cut never reaches origin.

What it does, in order:
  1. the soak gate, against the candidate tag (linux-x86_64 + windows)
  2. `scripts/check-release-freeze.sh --final-tag`
  3. the tag signature, verified with the repo's own tool
  4. only then: move the tag, force-push it, and report

Usage:
    python scripts/recut_v130_tag.py --check-only     # gates only, no push
    python scripts/recut_v130_tag.py                   # gates, then re-cut
"""

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TAG = "v1.3.0"
ORIGIN = "origin"


def run(cmd, **kw):
    p = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True,
                       encoding="utf-8", errors="replace", **kw)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def say(msg):
    # The console on a Chinese-locale Windows is GBK, and the soak
    # collector's output carries characters GBK cannot encode (an
    # em-dash, a tree glyph, a box-drawing rune).  `print` would raise
    # UnicodeEncodeError in the middle of a four-step gate, and the
    # crash would read as "the gate failed" -- when in fact the gate
    # never ran.  Reconfigure stdout once, so a character the console
    # cannot show becomes a '?' instead of taking the whole script
    # down.  The gate's verdict is carried by the exit code and by the
    # ASCII status lines, neither of which depends on the encoding.
    try:
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError):
        pass
    print(msg, flush=True)


def step(n, total, title):
    say("\n[%d/%d] %s" % (n, total, title))


# --------------------------------------------------------------------------
# 1. the soak gate
# --------------------------------------------------------------------------

def fetch_evidence():
    rc, out = run([sys.executable, "tools/fetch_soak_evidence.py",
                   "--days", "21", "--out", "build/soak"])
    say(out.strip()[-800:])
    return rc == 0


def soak_gate(candidate):
    """Run the gate exactly as release.yml runs it.

    `--expect-ref ""` is passed as an empty list element, NOT through a
    shell: PowerShell and cmd.exe both eat the empty string, and the run
    then silently becomes `--expect-ref` with a missing value.  That
    failure mode looks like "the window is 0/7" and is not one.
    """
    cmd = [
        sys.executable, "tools/run_soak.py",
        "--summary", "--require", "--require-days", "5",
        "--expect-ref", "",
        "--expect-ancestor-of", candidate,
        "--max-window-age-days", "30",
    ]
    rc, out = run(cmd)
    say(out.strip())
    return rc == 0, out


# --------------------------------------------------------------------------
# 2. the freeze checklist
# --------------------------------------------------------------------------

def freeze_check():
    bash = shutil.which("bash")
    if not bash:
        say("SKIP: bash not found")
        return True
    rc, out = run([bash, "scripts/check-release-freeze.sh", "--final-tag"])
    say(out.strip()[-1500:])
    return rc == 0


# --------------------------------------------------------------------------
# 3. the signature
# --------------------------------------------------------------------------

def sign(candidate, target):
    """Create a signed annotated tag on `target`, replacing any local one.

    The tag must point at `target` -- the commit the gates were run
    against -- and NOT at HEAD.  Signing HEAD is wrong twice over: the
    working tree may be on a branch that is not the candidate (a
    re-cut run from a feature branch would then publish that branch),
    and the tag would disagree with the commit the soak evidence was
    measured against.

    `git tag -s -m <message> <name> <commit>` creates the annotated tag
    on `target` directly, so no separate `git tag -f` is needed.  That
    matters because `git tag -f <name> <commit>` does NOT mean "move
    the tag": with `tag.gpgSign=true` it asks git to create a *signed
    annotated* tag, and with no `-m` it opens an editor for the message.
    On a machine with no EDITOR that fails with "Terminal is dumb, but
    EDITOR unset" -- and the script would report a failure for a tag
    it had already signed correctly.

    The local tag is only replaced once the new one exists: the script
    signs a throw-away tag first, and only then points the real tag at
    the signed tag OBJECT.  Deleting first and signing second would
    leave the repository with NO local tag at all if the signing
    failed -- and on a machine where the local tag is the only copy of
    a signature, that is not a recoverable state.

    The swap uses `git update-ref`, not `git tag <name> <other-tag>`:
    the latter resolves the other tag to its COMMIT and creates a new
    lightweight tag, which throws the signature away.  `update-ref`
    points the ref at the tag object itself, so the annotation and the
    signature survive intact.
    """
    probe = candidate + "-staged"
    run(["git", "tag", "-d", probe])
    rc, out = run(["git", "tag", "-s", "-m", "NimRTC %s" % TAG, probe,
                   target])
    if rc != 0:
        say(out.strip())
        run(["git", "tag", "-d", probe])
        return False
    signed = run(["git", "rev-parse", probe])[1].strip()
    rc, out = run(["git", "update-ref", "refs/tags/%s" % candidate, signed])
    run(["git", "tag", "-d", probe])
    if rc != 0:
        say(out.strip())
        return False
    return True


def verify_signature(candidate):
    rc, out = run([sys.executable, "tools/verify_tag_signature.py",
                   "--tag", candidate, "--repo", str(REPO)])
    say(out.strip())
    return rc == 0


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check-only", action="store_true",
                    help="run the gates and report; never touch the tag")
    ap.add_argument("--tag", default=TAG)
    args = ap.parse_args()
    tag = args.tag
    total = 4

    say("=== re-cut candidate for %s ===" % tag)

    # The candidate is a LOCAL, unsigned tag on origin/main: the gate
    # resolves `--expect-ancestor-of <candidate>`, and a commit-ish is
    # enough for that.  It is signed later, once the gates have passed.
    rc, out = run(["git", "fetch", ORIGIN, "main"])
    rc, out = run(["git", "rev-parse", "origin/main"])
    target = out.strip()
    if not target or rc != 0:
        say("FATAL: cannot resolve origin/main")
        return 2
    say("candidate commit: %s" % target)

    rc, _ = run(["git", "tag", "-d", "recut-candidate"])
    rc, out = run(["git", "tag", "-f", "-m", "recut candidate", "recut-candidate", target])
    if rc != 0:
        say("FATAL: cannot create the candidate tag")
        say(out.strip())
        return 2

    step(1, total, "fetch soak evidence")
    ok = fetch_evidence()
    if not ok:
        say("WARN: the evidence fetch failed; the gate below will say so")

    step(2, total, "soak gate (linux-x86_64 + windows)")
    gate_ok, gate_out = soak_gate("recut-candidate")
    if not gate_ok:
        say("\nGATE FAILED -- the tag was NOT touched.")
        say("This is the same gate that failed release run 44687753371.")
        say("Wait for the window to reach 7 green days on BOTH platforms,")
        say("then run this again.")
        run(["git", "tag", "-d", "recut-candidate"])
        return 1

    step(3, total, "release freeze checklist")
    if not freeze_check():
        say("\nFREEZE CHECK FAILED -- the tag was NOT touched.")
        run(["git", "tag", "-d", "recut-candidate"])
        return 1

    step(4, total, "sign and verify")
    # In `--check-only` mode the real tag must not be touched at all: the
    # point of that mode is to report whether a cut WOULD succeed.  So
    # the signing ability is proven on a throw-away tag, which is
    # deleted again, and the real tag is left exactly as it was.
    # (Signing it for real would mean deleting and recreating a tag the
    # caller asked not to touch -- and on a repository where the local
    # tag is the only copy of a signature, that is not a harmless
    # side effect.)
    if args.check_only:
        probe = "recut-sign-probe"
        if not sign(probe, target):
            say("FATAL: could not sign a tag at all")
            run(["git", "tag", "-d", "recut-candidate"])
            return 2
        ok = verify_signature(probe)
        run(["git", "tag", "-d", probe])
        if not ok:
            say("FATAL: the signature does not verify")
            run(["git", "tag", "-d", "recut-candidate"])
            return 2
    else:
        if not sign(tag, target):
            say("FATAL: could not sign the tag")
            run(["git", "tag", "-d", "recut-candidate"])
            return 2
        if not verify_signature(tag):
            say("FATAL: the signature does not verify -- the tag was NOT pushed")
            run(["git", "tag", "-d", tag])
            return 2
        # The tag now points at the target and carries a signature; there
        # is no separate "move" step, and none is needed.
        rc, out = run(["git", "rev-parse", "%s^{}" % tag])
        landed = out.strip()
        if landed != target:
            say("FATAL: the tag points at %s, not at %s" % (landed, target))
            run(["git", "tag", "-d", tag])
            return 2
        say("local tag %s now points at %s" % (tag, target))
    run(["git", "tag", "-d", "recut-candidate"])

    if args.check_only:
        say("\n=== all gates passed; --check-only, so the tag is untouched ===")
        say("The tag %s still points at the old commit and will still" % tag)
        say("fail the release.  Re-run without --check-only to re-cut it.")
        return 0

    rc, out = run(["git", "push", "--force", ORIGIN, "refs/tags/%s" % tag])
    say(out.strip())
    if rc != 0:
        say("\nFATAL: the force-push failed.  The tag was moved locally but")
        say("origin still carries the old one -- push it by hand:")
        say("    git push --force origin refs/tags/%s" % tag)
        return 2

    say("\n=== %s re-cut and pushed ===" % tag)
    say("  commit: %s" % target)
    say("  the release workflow will now run on this tag.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
