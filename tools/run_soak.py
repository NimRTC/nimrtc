#!/usr/bin/env python3
# Copyright (c) 2026 NimRTC contributors.
# SPDX-License-Identifier: MIT
"""Collect the daily evidence for the NimRTC soak window.

What this tool is for
---------------------
The release gate requires consecutive days of green end-to-end
acceptance runs (Case D and Case E, on Windows and Linux x86_64) before
the release tag is cut.  This tool is the evidence recorder for that
window: it runs the acceptance runner once, writes a machine-readable
record of what happened, and -- over a stretch of daily invocations --
lets `run_soak.py --summary` state plainly whether the window is
satisfied.

It runs one day at a time on purpose.  A soak that only starts when
somebody remembers to start it is not a soak; the intended deployment
is a Windows Scheduled Task and a Linux cron entry, each invoking this
script once a day.  See `docs/plan/soak-runbook.md`.

Honesty rules baked into this tool
----------------------------------
The soak's value is entirely in its evidence, so the tool is built to
make a green result hard to fake:

  * A run is PASS only when the acceptance runner itself exits zero.
    Anything else is FAIL.
  * A run whose preconditions were not met -- no build artefacts, no
    Chrome, no Linux peer -- is recorded as INCONCLUSIVE, never as
    PASS.  An inconclusive day does not count towards the window.
  * The streak counts *calendar*-consecutive green days.  A day with
    no evidence, an inconclusive day, and a failing day all end the
    streak.  The release gate demands CONSECUTIVE days, so green days
    spread across three weeks must not read as a streak -- that would
    let a tag be cut on a window the gate never actually authorised.
  * A day is green when every required case has at least one PASS.
    A day is normally measured more than once -- the CI job and the
    scheduled local task both write the same platform/day -- so one
    FAIL among passing runs is evidence about the runner, not about
    the program, and does not spoil the day.  A case that failed
    *every* time it ran has no passing run at all, and that day is
    not green.  Every FAIL is still reported.
  * The summary prints the streak it can actually prove, and separately
    prints the days without evidence and the inconclusive days, so a
    window with gaps cannot be mistaken for a clean one.
  * Every record carries the tree it was measured on (`git_ref` and
    `git_commit`), and a day only counts towards the window when it was
    measured on the candidate tree.  Two rules express that, and the
    release gate uses the second one:
      - `--expect-ref` -- the day's `git_ref` must be exactly this
        string.  The strictest form, and the wrong one for a window
        measured on a branch that moves.
      - `--expect-ancestor-of` -- the commit the day was measured on
        must be an ANCESTOR of the candidate commit.  This is what
        makes a window measured on `main` usable for a tag cut later:
        the tagged tree descends from every commit that was measured.
        A day measured on a tree the candidate does NOT descend from
        proves nothing about it, and is not green.

Usage
-----
    python tools/run_soak.py --platform windows --case d,e
    python tools/run_soak.py --platform linux-x86_64 --case d,e
    python tools/run_soak.py --summary
    python tools/run_soak.py --summary --json
    python tools/run_soak.py --summary --expect-ref release/1.1
    python tools/run_soak.py --summary --expect-ancestor-of v1.3.0 \
        --max-window-age-days 30

The default out-dir is `build/soak/`.  Evidence never goes to the
project root: the root is kept free of scratch files by policy, and a
soak log landing there would show up in the release freeze checklist's
`root` check.

Exit codes
----------
    0  the run completed and passed, or --summary succeeded
    1  the run failed, or the summary found the window unsatisfied
    2  the tool could not run (runner missing, bad arguments, ...)
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timedelta, timezone
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_OUT_DIR = REPO_ROOT / "build" / "soak"
DEFAULT_RUNNER = REPO_ROOT / "tools" / "run_e2e_acceptance.py"

# The acceptance runner is invoked as `python <runner> --case X`, and
# the interpreter it runs under is not always the same one: on Windows
# the runner is normally launched with `python`, on a Linux CI image
# with `python3`, and some minimal images have only one of the two.
# Probe by running the candidate rather than by asking the shell
# whether it is on PATH -- on a stock Windows box `python3` resolves to
# the Microsoft Store app-execution alias, which is "present" but
# cannot execute anything.
PYTHON_CANDIDATES = ("python3", "python")

# stdout/stderr excerpts are truncated to this many bytes.  The full
# output of a failed acceptance run can be large (Chrome logs, DTLS
# transcripts), and an unbounded excerpt would make the evidence file
# unreadable -- but the beginning of the output is where the failure
# reason lives.
EXCERPT_BYTES = 8 * 1024

PLATFORMS = ("windows", "linux-x86_64")

# The tree the v1.1.0 window is a statement about.  The tag is cut from
# `release/1.1`, so a day measured on `main` -- which already carries the
# v1.2.0 audio body -- is evidence about a different program and is not
# credited to the window.
DEFAULT_EXPECT_REF = "release/1.1"

# The looser form of the same rule: a day counts when the commit it was
# measured on is an ANCESTOR of the tree that gets tagged.  This is what
# lets a window be measured on a moving branch (`main`) while still
# proving that the tagged tree descends from what was measured.
#
# It is deliberately a *separate* switch rather than a replacement for
# `--expect-ref`: the two rules answer different questions, and a
# release that wants the stronger guarantee has to ask for it by name.
# `--expect-ancestor-of` is a commit-ish, not a branch name.


class ToolError(Exception):
    """Raised when the tool itself cannot run (exit code 2)."""


def decode(data) -> str:
    if isinstance(data, bytes):
        return data.decode("utf-8", errors="replace")
    return data or ""


def find_python():
    """Return an interpreter that can actually execute code."""
    import shutil
    for candidate in PYTHON_CANDIDATES:
        path = shutil.which(candidate)
        if not path:
            continue
        try:
            probe = subprocess.run(
                [path, "-c", "import sys"],
                capture_output=True, stdin=subprocess.DEVNULL, timeout=30)
        except (OSError, subprocess.TimeoutExpired):
            continue
        if probe.returncode == 0:
            return path
    return None


def local_platform() -> str:
    """Map the running machine onto one of the soak platforms."""
    system = platform.system().lower()
    if system == "windows":
        return "windows"
    if system == "linux":
        machine = platform.machine().lower()
        if machine in ("x86_64", "amd64"):
            return "linux-x86_64"
        return "linux-%s" % machine
    return "%s-%s" % (system, platform.machine().lower())


# ---------------------------------------------------------------------------
# Preconditions
# ---------------------------------------------------------------------------

def check_preconditions(case, plat, runner):
    """Report whether this machine can produce a meaningful result.

    Returns (ready, reason).  `reason` explains what is missing; it is
    recorded verbatim in the evidence file so that a reader can tell an
    inconclusive day apart from a failing one without re-running
    anything.
    """
    if not runner.is_file():
        return False, "acceptance runner not found: %s" % runner

    # The acceptance runner honours NIMRTC_SKIP_CASE_D by writing a
    # synthetic PASS for Case D without running anything (a historical
    # CI escape hatch for branches that predate the DTLS handshake
    # reassembly fix).  A synthetic PASS is indistinguishable from a
    # real one in the runner's exit code, so the only defence is to
    # refuse to run while the flag is set.  Recording one as a soak
    # day would put a fabricated green into a release gate.
    if case == "d" and os.environ.get("NIMRTC_SKIP_CASE_D", "0") == "1":
        return False, ("NIMRTC_SKIP_CASE_D=1 is set, so the acceptance runner "
                       "would report a synthetic PASS for Case D; unset it "
                       "before collecting soak evidence")

    # The runner locates its binaries under the build tree, honouring
    # NIMRTC_BUILD_DIR for out-of-tree builds.  Look for the same
    # artefacts it does rather than for "any .exe anywhere": a stray
    # executable in an unrelated directory is not evidence that the
    # project under test was built.
    build_dir = Path(os.environ.get("NIMRTC_BUILD_DIR",
                                    str(REPO_ROOT / "build")))

    def resolve(cfg, sub):
        for candidate in (build_dir / sub / cfg, build_dir / cfg / sub,
                          build_dir / sub):
            if candidate.is_dir():
                return candidate
        return build_dir / sub / cfg

    if case == "d":
        examples = resolve("Debug", "examples")
        tests = resolve("Debug", "tests")
        wanted = [
            examples / ("demo-p2p.exe" if plat == "windows" else "demo-p2p"),
            tests / ("test_dtls_inproc.exe" if plat == "windows"
                     else "test_dtls_inproc"),
        ]
        missing = [str(path) for path in wanted if not path.exists()]
        if missing:
            return False, ("required build artefacts are missing: %s; build "
                           "the project before running Case D"
                           % ", ".join(missing))

        # Case D drives a real Chrome through Playwright, so Playwright
        # and its Chromium build have to be importable.
        try:
            import playwright  # noqa: F401
        except Exception:
            return False, ("the `playwright` package is not importable; Case D "
                           "drives a real Chrome peer through Playwright")
    return True, "preconditions satisfied"


def shutil_which(name):
    import shutil
    return shutil.which(name)


# ---------------------------------------------------------------------------
# Evidence records
# ---------------------------------------------------------------------------

def git_provenance():
    """Return the tree the evidence is being measured on.

    The window is a statement about a *program*, and the program is the
    tree the collector runs against.  Recording the branch and the
    commit makes each day's evidence self-describing: a reader can see
    exactly which tree produced a green day, and the summary can refuse
    to credit a day that was measured on something other than the
    candidate branch.

    Returns a dict with `git_ref`, `git_commit` and `git_dirty`.  The
    first two are `None` when the tree cannot be identified (not a git
    checkout, no `git` on PATH) -- the record is still written, and the
    summary then treats the day as measured on an unknown tree, which
    is not green.

    `git_dirty` says whether the tree carried uncommitted changes when
    the measurement was taken.  A commit identifies a *tree*, but a
    dirty tree is not that tree: the program that ran is whatever the
    working directory happened to contain.  Recording the flag and
    refusing to credit a dirty day keeps the window a statement about
    the tree that gets tagged.
    """
    provenance = {"git_ref": None, "git_commit": None, "git_dirty": None}

    # The branch the tree is on.  `symbolic-ref` answers the branch when
    # HEAD is attached to one, and fails when the checkout is detached
    # -- which is what a CI runner's `actions/checkout` may leave
    # behind.  In that case the branch that points at HEAD is looked up
    # instead, so a CI-produced record names the same tree a local one
    # does rather than reporting an unidentified tree.
    try:
        proc = subprocess.run(
            ["git", "-C", str(REPO_ROOT), "symbolic-ref", "--short", "HEAD"],
            capture_output=True, stdin=subprocess.DEVNULL, timeout=30)
        if proc.returncode == 0:
            value = decode(proc.stdout).strip()
            if value:
                provenance["git_ref"] = value
    except (OSError, subprocess.TimeoutExpired):
        pass

    if provenance["git_ref"] is None:
        try:
            proc = subprocess.run(
                ["git", "-C", str(REPO_ROOT), "for-each-ref",
                 "--points-at=HEAD", "--format=%(refname:short)", "refs/heads/"],
                capture_output=True, stdin=subprocess.DEVNULL, timeout=30)
            if proc.returncode == 0:
                branches = [line.strip() for line in decode(proc.stdout).splitlines()
                            if line.strip()]
                if branches:
                    provenance["git_ref"] = branches[0]
        except (OSError, subprocess.TimeoutExpired):
            pass

    try:
        proc = subprocess.run(
            ["git", "-C", str(REPO_ROOT), "rev-parse", "HEAD"],
            capture_output=True, stdin=subprocess.DEVNULL, timeout=30)
        if proc.returncode == 0:
            value = decode(proc.stdout).strip()
            if value:
                provenance["git_commit"] = value
    except (OSError, subprocess.TimeoutExpired):
        pass

    # Whether the working tree matched that commit.  `--porcelain`
    # prints one line per difference and nothing at all when the tree
    # is clean; ignored files are not reported, so the build tree
    # under `build/` does not count as a difference.  A record written
    # by a collector that could not answer the question carries `None`,
    # which the summary treats as "not known to be clean" -- the same
    # conservative reading as an unidentified tree.
    try:
        proc = subprocess.run(
            ["git", "-C", str(REPO_ROOT), "status", "--porcelain"],
            capture_output=True, stdin=subprocess.DEVNULL, timeout=60)
        if proc.returncode == 0:
            provenance["git_dirty"] = bool(decode(proc.stdout).strip())
    except (OSError, subprocess.TimeoutExpired):
        pass

    return provenance


def evidence_path(out_dir, plat, stamp):
    """Path of the JSONL file holding one day's records."""
    day = stamp.strftime("%Y-%m-%d")
    return out_dir / ("%s-%s.jsonl" % (plat, day))


def append_record(path, record):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(record, sort_keys=True) + "\n")


def run_once(runner, plat, case, out_dir, python_bin=None):
    """Run one acceptance case and record the outcome."""
    stamp = datetime.now(timezone.utc)
    interpreter = python_bin or find_python()
    if interpreter is None:
        raise ToolError("no usable python interpreter found")

    ready, reason = check_preconditions(case, plat, runner)
    provenance = git_provenance()

    if not ready:
        record = {
            "timestamp": stamp.isoformat(),
            "platform": plat,
            "case": case,
            "status": "INCONCLUSIVE",
            "reason": reason,
            "command": None,
            "exit_code": None,
            "duration_seconds": 0.0,
            "stdout_excerpt": "",
            "stderr_excerpt": "",
            "git_ref": provenance["git_ref"],
            "git_commit": provenance["git_commit"],
            "git_dirty": provenance["git_dirty"],
        }
        append_record(evidence_path(out_dir, plat, stamp), record)
        return record

    cmd = [interpreter, str(runner), "--case", case]
    started = time.time()
    try:
        proc = subprocess.run(
            cmd,
            cwd=str(REPO_ROOT),
            capture_output=True,
            stdin=subprocess.DEVNULL,
            timeout=60 * 30,
        )
        stdout = decode(proc.stdout)
        stderr = decode(proc.stderr)
        exit_code = proc.returncode
    except subprocess.TimeoutExpired as exc:
        stdout = decode(exc.stdout)
        stderr = decode(exc.stderr)
        exit_code = None
    except OSError as exc:
        stdout = ""
        stderr = str(exc)
        exit_code = None
    duration = round(time.time() - started, 3)

    if exit_code is None:
        status = "FAIL"
        reason = "the acceptance runner did not complete (timeout or launch error)"
    elif exit_code == 0:
        status = "PASS"
        reason = ""
    else:
        status = "FAIL"
        reason = "the acceptance runner exited %d" % exit_code

    record = {
        "timestamp": stamp.isoformat(),
        "platform": plat,
        "case": case,
        "status": status,
        "reason": reason,
        "command": " ".join(shlex.quote(part) for part in cmd),
        "exit_code": exit_code,
        "duration_seconds": duration,
        "stdout_excerpt": stdout[:EXCERPT_BYTES],
        "stderr_excerpt": stderr[:EXCERPT_BYTES],
        "git_ref": provenance["git_ref"],
        "git_commit": provenance["git_commit"],
        "git_dirty": provenance["git_dirty"],
    }
    append_record(evidence_path(out_dir, plat, stamp), record)
    return record


# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

def load_records(out_dir):
    """Load every evidence record, newest last.

    Exact duplicates are dropped.  The merge procedure in the runbook
    APPENDS the CI-produced records into `build/soak/`, so running it
    twice puts the same record in the file twice.  A duplicate is not
    new evidence -- it is the same evidence written twice -- and it
    would otherwise inflate the failure count and skew the summary.
    """
    records = []
    seen = set()
    if not out_dir.is_dir():
        return records
    for path in sorted(out_dir.glob("*.jsonl")):
        with path.open("r", encoding="utf-8") as handle:
            for line_number, line in enumerate(handle, 1):
                line = line.strip()
                if not line:
                    continue
                try:
                    record = json.loads(line)
                except json.JSONDecodeError:
                    # A truncated line means the writer died mid-record.
                    # Skip it rather than fail the whole summary: a
                    # partial day is still a day with no complete
                    # evidence, and the summary will show the gap.
                    sys.stderr.write(
                        "warning: skipping malformed record %s:%d\n"
                        % (path.name, line_number))
                    continue
                key = json.dumps(record, sort_keys=True)
                if key in seen:
                    continue
                seen.add(key)
                records.append(record)
    records.sort(key=lambda item: item.get("timestamp", ""))
    return records


def _day_is_green(cases, required_cases):
    """Decide whether one day of evidence counts as green.

    `cases` maps a case name to the list of records for that case on
    that day.  The rule is:

    * every required case must have at least one PASS record, and
    * a required case whose every run FAILED does not count, even if a
      PASS exists for it under a different name.

    The first clause is what makes a day green; the second is what it
    says about a case that failed every time it ran.

    A FAIL does not by itself spoil the day.  A day is normally measured
    several times -- the CI job runs it, the scheduled local task runs
    it, and either may be re-run after a fix -- and a single FAIL among
    passing runs is evidence about the *runner*, not about the program:
    a Chrome interop that hits the 60-second timeout on a shared CI
    machine, on a commit that passed the same case three times earlier
    that day, is a flaky measurement.  Demoting the whole day for it
    measured the runner's mood rather than the build's stability.

    What still disqualifies a day is a case that failed *every* time it
    ran: then no passing run exists for that case, the first clause
    already rejects it, and the day carries no evidence that the case
    works at all.

    An INCONCLUSIVE record is harmless in the same way -- running twice
    is harmless -- and otherwise the day simply lacks a PASS for that
    case and is not green.

    Returns True when the day counts towards the streak.
    """
    for case in required_cases:
        records = cases.get(case, [])
        if not any(rec.get("status") == "PASS" for rec in records):
            return False
    return True


def _day_refs(cases):
    """The distinct trees a day's records were measured on.

    A record with no `git_ref` (written before the collector recorded
    the tree, or produced from a checkout that could not be
    identified) reports `None`, which is shown as `unknown`.
    """
    refs = set()
    for records in cases.values():
        for rec in records:
            refs.add(rec.get("git_ref") or "unknown")
    return sorted(refs)


def _day_is_on_ref(cases, expect_ref):
    """Whether every record of a day was measured on `expect_ref`.

    A day measured on any other tree is not evidence about the
    release candidate, so it cannot be green.  This is the rule that
    stops a green day taken on `main` from being credited to a window
    whose candidate lives on `release/1.1`.
    """
    if not expect_ref:
        return True
    for records in cases.values():
        for rec in records:
            if rec.get("git_ref") != expect_ref:
                return False
    return True


def _git_ancestors(commit, candidate):
    """Whether `commit` is an ancestor of (or equal to) `candidate`.

    This is the ancestry form of the "which tree" rule.  It answers a
    different question from `_day_is_on_ref`: not "was the day measured
    on this branch", but "does the tree that gets tagged descend from
    the tree the day was measured on".

    The second question is the one a moving-branch release needs.  A
    window measured on `main` while `main` advanced is still evidence
    about the release, provided the tag is cut from a descendant of
    every commit that was measured -- the tag then contains everything
    the window proved, plus whatever came after it.

    The converse is what makes the rule safe rather than permissive:
    a day measured on a tree that the candidate does NOT descend from
    proves nothing about the candidate.  That is the case the exact
    branch-name rule catches too, and it is the case that made the
    `release/1.1` window worthless once `release/1.1` fell two
    generations behind `main`.

    Returns None when the question cannot be answered -- a record with
    no commit, a repository that does not contain the commit, or a git
    that is not there -- because "cannot tell" must not be read as
    "yes".
    """
    if not commit or not candidate:
        return None
    try:
        proc = subprocess.run(
            ["git", "-C", str(REPO_ROOT), "merge-base", "--is-ancestor",
             commit, candidate],
            capture_output=True, stdin=subprocess.DEVNULL, timeout=60)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if proc.returncode == 0:
        return True
    if proc.returncode == 1:
        return False
    # 128: the object is unknown to this repository.  Not an answer.
    return None


def resolve_commitish(ref):
    """Resolve a commit-ish (SHA, tag, branch) into a commit SHA.

    Returns None when the ref cannot be resolved -- an unknown branch,
    a repository that does not contain it, or no git at all.  The caller
    treats None as "the question cannot be answered", which is not the
    same as "yes".
    """
    if not ref:
        return None
    try:
        proc = subprocess.run(
            ["git", "-C", str(REPO_ROOT), "rev-parse", "--verify",
             "%s^{commit}" % ref],
            capture_output=True, stdin=subprocess.DEVNULL, timeout=60)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if proc.returncode != 0:
        return None
    value = decode(proc.stdout).strip()
    return value or None


def commit_date(commit):
    """The committer date of a commit, as a `YYYY-MM-DD` string.

    Returns None when the date cannot be read.  The date is what makes
    "the window must be recent" checkable: a seven-day streak that
    ended six months ago is not evidence about today's release, however
    green it was.
    """
    if not commit:
        return None
    try:
        proc = subprocess.run(
            ["git", "-C", str(REPO_ROOT), "show", "-s", "--format=%cs", commit],
            capture_output=True, stdin=subprocess.DEVNULL, timeout=60)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if proc.returncode != 0:
        return None
    value = decode(proc.stdout).strip()
    return value or None


def stale_platforms(summary, reference_day, max_age_days):
    """The platforms whose window ended too long before `reference_day`.

    The streak counts seven *consecutive* green days, but it says
    nothing about WHEN they were.  A window that closed in March is
    not evidence about a release cut in November -- the code has moved
    since.  So the gate also requires the window to have closed
    recently.

    `reference_day` is a `YYYY-MM-DD` string (the candidate commit's
    date).  Returns a list of "platform (last green day, age in days)"
    strings, empty when every window is recent enough.
    """
    stale = []
    for plat in PLATFORMS:
        info = summary.get(plat, {})
        green_days = info.get("green_days") or []
        if not green_days:
            # A platform with no green day at all is already unsatisfied
            # by the streak rule; it is not reported here as well, so
            # the two reasons are not confused.
            continue
        last = max(green_days)
        try:
            age = (datetime.strptime(reference_day, "%Y-%m-%d")
                   - datetime.strptime(last, "%Y-%m-%d")).days
        except (TypeError, ValueError):
            continue
        if age > max_age_days:
            stale.append("%s (last green day %s, %d days before the "
                         "candidate)" % (plat, last, age))
    return stale


def _day_is_ancestor_of(cases, candidate, resolver=None):
    """Whether every record of a day was measured on an ancestor of `candidate`.

    Returns a verdict of "yes", "no" or "unknown":

    * "yes"    -- every record carries a commit that is an ancestor of
      the candidate.  The day is evidence about the tagged tree.
    * "no"     -- at least one record carries a commit that is NOT an
      ancestor, or carries no commit at all.  The day proves nothing
      about the candidate.
    * "unknown" -- the question could not be answered for at least one
      record (the commit is not in this repository, or git failed).

    "unknown" is kept apart from "no" on purpose: a day measured on a
    commit this clone does not have is a *readable* record whose
    relation to the candidate is unknown, and the gate must not treat
    it as either evidence or a failure.  The summary reports it, and
    the release gate treats it as NOT green -- the same conservative
    reading as an unidentified tree.
    """
    if not candidate:
        # The rule is not in force (no `--expect-ancestor-of` was
        # given), so it raises no objection.  This is different from
        # the "unknown" verdict below, which is what happens when the
        # rule IS in force but the question could not be answered.
        return "yes"
    resolve = resolver or _git_ancestors
    verdicts = []
    for records in cases.values():
        for rec in records:
            commit = rec.get("git_commit")
            if not commit:
                # A record that does not say which commit it was
                # measured on cannot be placed in the candidate's
                # history at all.  It is not an ancestor of anything.
                verdicts.append("unknown")
                continue
            verdict = resolve(commit, candidate)
            if verdict is None:
                verdicts.append("unknown")
            elif verdict:
                verdicts.append("yes")
            else:
                verdicts.append("no")
    if not verdicts:
        return "unknown"
    if "no" in verdicts:
        return "no"
    if "unknown" in verdicts:
        return "unknown"
    return "yes"


def _day_is_clean(cases):
    """Whether every record of a day was measured on a clean tree.

    A commit identifies a tree, but a dirty working tree is not that
    tree: the program that produced the evidence is whatever the
    working directory happened to contain.  A record that does not say
    whether the tree was clean (one written before the collector
    tracked it) counts as NOT clean -- the same conservative reading as
    an unidentified tree, because the gate cannot certify what it
    cannot see.
    """
    for records in cases.values():
        for rec in records:
            if rec.get("git_dirty") is not False:
                return False
    return True


def _day_cleanliness(cases):
    """How well the day's records describe the tree they were measured on.

    Returns "clean", "dirty" or "unknown":

    * "dirty"   -- at least one record saw uncommitted changes;
    * "unknown" -- no record saw a dirty tree, but at least one did not
      say whether the tree was clean (a record written before the
      collector tracked the flag);
    * "clean"   -- every record saw a clean tree.
    """
    seen = set()
    for records in cases.values():
        for rec in records:
            dirty = rec.get("git_dirty")
            if dirty is True:
                seen.add("dirty")
            elif dirty is False:
                seen.add("clean")
            else:
                seen.add("unknown")
    if "dirty" in seen:
        return "dirty"
    if "unknown" in seen:
        return "unknown"
    return "clean"


def summarise(records, required_cases, expect_ref=None,
              expect_ancestor_of=None, resolver=None):
    """Compute the per-platform streaks and the gaps.

    `expect_ref` is the tree the window is a statement about.  When it
    is given, a day only counts as green if every one of its records
    was measured on that tree.

    `expect_ancestor_of` is the ancestry form of the same rule: a day
    counts as green when the commit it was measured on is an ancestor
    of the candidate commit.  It is the rule a release cut from a
    moving branch needs -- see `_day_is_ancestor_of`.

    Both may be given at once, in which case a day has to satisfy
    BOTH.  That is the strictest reading, and it is the one the
    release gate uses: the window must have been measured on the
    candidate branch AND on a tree the candidate descends from.
    """
    per_platform = {}
    for record in records:
        per_platform.setdefault(record.get("platform", "?"), []).append(record)

    summary = {}
    for plat, items in per_platform.items():
        # day -> case -> [records].
        #
        # A day can hold more than one record for the same case: the
        # local Scheduled Task and the CI job both write the same
        # platform/day, and a day may be re-run after a fix.  Every
        # record is kept, because a summary that keeps only the last
        # one cannot report a failure that a later PASS hid.
        days = {}
        for item in items:
            day = item.get("timestamp", "")[:10]
            days.setdefault(day, {}).setdefault(item.get("case"), []).append(item)

        # Classify every day with evidence into green / not-green.
        green_dates = []
        not_green = []
        wrong_ref_days = []
        dirty_days = []
        unknown_days = []
        unrelated_days = []
        undecidable_days = []
        for day in sorted(days):
            cases = days[day]
            on_ref = _day_is_on_ref(cases, expect_ref)
            if not on_ref:
                wrong_ref_days.append({"day": day, "refs": _day_refs(cases)})
            ancestry = _day_is_ancestor_of(cases, expect_ancestor_of,
                                           resolver=resolver)
            if ancestry == "no":
                unrelated_days.append({"day": day, "refs": _day_refs(cases)})
            elif ancestry == "unknown":
                undecidable_days.append({"day": day, "refs": _day_refs(cases)})
            cleanliness = _day_cleanliness(cases)
            if cleanliness == "dirty":
                dirty_days.append({"day": day})
            elif cleanliness == "unknown":
                unknown_days.append({"day": day})
            green = (_day_is_green(cases, required_cases)
                     and on_ref
                     and cleanliness == "clean"
                     and ancestry != "no"
                     and ancestry != "unknown")
            if green:
                green_dates.append(day)
            else:
                entry = {
                    "day": day,
                    "cases_without_pass": [
                        case for case in required_cases
                        if not any(rec.get("status") == "PASS"
                                   for rec in cases.get(case, []))],
                }
                if not on_ref:
                    # The cases may well have passed; what disqualifies the
                    # day is the tree it was measured on, and the report
                    # has to say so rather than imply a case failure.
                    entry["wrong_ref"] = _day_refs(cases)
                if ancestry == "no":
                    entry["unrelated_tree"] = _day_refs(cases)
                elif ancestry == "unknown":
                    entry["undecidable_tree"] = _day_refs(cases)
                if cleanliness != "clean":
                    entry["tree_state"] = cleanliness
                not_green.append(entry)

        # The streak is a run of *calendar*-consecutive green days.
        #
        # Counting green days that merely happen to be adjacent in the
        # sorted list would let a window of five green days spread
        # across three weeks masquerade as a five-day streak -- and the
        # release gate is seven CONSECUTIVE days.  A missing day
        # therefore ends the streak, exactly like a failing or an
        # inconclusive day does.
        streak = 0
        best = 0
        previous = None
        for day in green_dates:
            if previous is not None:
                gap = (datetime.strptime(day, "%Y-%m-%d")
                       - datetime.strptime(previous, "%Y-%m-%d")).days
                if gap != 1:
                    streak = 0
            streak += 1
            best = max(best, streak)
            previous = day

        inconclusive = [
            {"day": day,
             "cases": sorted(case for case, recs in cases.items()
                             if any(r.get("status") == "INCONCLUSIVE" for r in recs)),
             "reasons": sorted({r.get("reason", "")
                                for recs in cases.values()
                                for r in recs
                                if r.get("status") == "INCONCLUSIVE"})}
            for day, cases in sorted(days.items())
            if any(r.get("status") == "INCONCLUSIVE"
                   for recs in cases.values() for r in recs)
        ]

        failures = [
            {"day": day, "case": case, "reason": rec.get("reason", ""),
             "timestamp": rec.get("timestamp", "")}
            for day, cases in sorted(days.items())
            for case, recs in sorted(cases.items())
            for rec in recs
            if rec.get("status") == "FAIL"
        ]

        # Days with no evidence at all, but which sit between the first
        # and the last recorded day.  These are the gaps that break a
        # streak, and they are listed explicitly so a reader can see
        # *why* the streak is short rather than having to diff the
        # dates by hand.
        gaps = []
        if days:
            first = min(days)
            last = max(days)
            cursor = datetime.strptime(first, "%Y-%m-%d")
            end = datetime.strptime(last, "%Y-%m-%d")
            while cursor <= end:
                key = cursor.strftime("%Y-%m-%d")
                if key not in days:
                    gaps.append(key)
                cursor += timedelta(days=1)

        summary[plat] = {
            "days_with_evidence": len(days),
            "consecutive_green_days": streak,
            "best_streak": best,
            "green_days": green_dates,
            "days_without_evidence": gaps,
            "inconclusive_days": inconclusive,
            "failures": failures,
            "not_green_days": not_green,
            "measured_refs": sorted({ref
                                     for day_cases in days.values()
                                     for ref in _day_refs(day_cases)}),
            "wrong_ref_days": wrong_ref_days,
            "dirty_tree_days": dirty_days,
            "unknown_cleanliness_days": unknown_days,
            "unrelated_tree_days": unrelated_days,
            "undecidable_tree_days": undecidable_days,
        }

    # Every tracked platform appears in the summary, even when it has
    # no evidence at all.  A platform that is simply absent from the
    # report reads as "not tracked", which is exactly the
    # misunderstanding the release gate has to prevent.
    for plat in PLATFORMS:
        summary.setdefault(plat, {
            "days_with_evidence": 0,
            "consecutive_green_days": 0,
            "best_streak": 0,
            "green_days": [],
            "days_without_evidence": [],
            "inconclusive_days": [],
            "failures": [],
            "not_green_days": [],
            "measured_refs": [],
            "wrong_ref_days": [],
            "dirty_tree_days": [],
            "unknown_cleanliness_days": [],
            "unrelated_tree_days": [],
            "undecidable_tree_days": [],
        })
    return summary


def print_summary(summary, required_cases, expect_ref=None,
                  expect_ancestor_of=None):
    print("soak summary (cases: %s)" % ", ".join(required_cases))
    if expect_ref:
        print("window is measured on: %s" % expect_ref)
    if expect_ancestor_of:
        print("window must descend from: %s" % expect_ancestor_of)
    print()
    for plat in sorted(summary):
        info = summary[plat]
        print("%s:" % plat)
        print("    days with evidence    : %d" % info["days_with_evidence"])
        print("    consecutive green days: %d" % info["consecutive_green_days"])
        print("    best streak so far    : %d" % info["best_streak"])
        if info.get("measured_refs"):
            print("    trees measured        : %s"
                  % ", ".join(info["measured_refs"]))
        if info.get("wrong_ref_days"):
            print("    days measured on another tree: %d (these do not count "
                  "as green)" % len(info["wrong_ref_days"]))
            for entry in info["wrong_ref_days"]:
                print("        %s  measured on: %s"
                      % (entry["day"], ", ".join(entry["refs"])))
        if info.get("dirty_tree_days"):
            print("    days measured on an unclean tree: %d (these do not count "
                  "as green)" % len(info["dirty_tree_days"]))
            for entry in info["dirty_tree_days"]:
                print("        %s  the working tree carried uncommitted changes"
                      % entry["day"])
        if info.get("unknown_cleanliness_days"):
            print("    days whose tree state was not recorded: %d (these do not "
                  "count as green)" % len(info["unknown_cleanliness_days"]))
            for entry in info["unknown_cleanliness_days"]:
                print("        %s  the collector did not record whether the "
                      "working tree was clean" % entry["day"])
        if info.get("unrelated_tree_days"):
            print("    days measured on a tree the candidate does NOT descend "
                  "from: %d (these do not count as green)"
                  % len(info["unrelated_tree_days"]))
            for entry in info["unrelated_tree_days"]:
                print("        %s  measured on: %s"
                      % (entry["day"], ", ".join(entry["refs"])))
        if info.get("undecidable_tree_days"):
            print("    days whose relation to the candidate is unknown: %d "
                  "(these do not count as green)"
                  % len(info["undecidable_tree_days"]))
            for entry in info["undecidable_tree_days"]:
                print("        %s  measured on: %s (the commit is not "
                      "resolvable here)" % (entry["day"], ", ".join(entry["refs"])))
        if info["inconclusive_days"]:
            print("    inconclusive days     : %d (these do not count as green)"
                  % len(info["inconclusive_days"]))
            for entry in info["inconclusive_days"]:
                print("        %s  %s" % (entry["day"], "; ".join(entry["reasons"])))
        if info.get("days_without_evidence"):
            print("    days without evidence : %d (these break the streak)"
                  % len(info["days_without_evidence"]))
            print("        %s" % ", ".join(info["days_without_evidence"]))
        if info["failures"]:
            print("    failures              : %d" % len(info["failures"]))
            for entry in info["failures"]:
                print("        %s  %s  %s" % (entry["day"], entry["case"], entry["reason"]))
        if info.get("not_green_days"):
            print("    days with evidence but not green: %d"
                  % len(info["not_green_days"]))
            for entry in info["not_green_days"]:
                if entry.get("wrong_ref"):
                    print("        %s  measured on another tree: %s"
                          % (entry["day"], ", ".join(entry["wrong_ref"])))
                    continue
                if entry.get("unrelated_tree"):
                    print("        %s  measured on a tree the candidate does "
                          "not descend from: %s"
                          % (entry["day"], ", ".join(entry["unrelated_tree"])))
                    continue
                if entry.get("undecidable_tree"):
                    print("        %s  the tree's relation to the candidate "
                          "could not be established: %s"
                          % (entry["day"], ", ".join(entry["undecidable_tree"])))
                    continue
                if entry.get("tree_state") == "dirty":
                    print("        %s  measured on an unclean tree" % entry["day"])
                    continue
                if entry.get("tree_state") == "unknown":
                    print("        %s  the tree's cleanliness was not recorded"
                          % entry["day"])
                    continue
                missing = ", ".join(entry["cases_without_pass"]) or "-"
                print("        %s  no PASS for: %s" % (entry["day"], missing))
        print()


# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------

def _record(day, case, status, plat="windows", reason="", ref=None, commit=None,
            dirty=False):
    """Build one synthetic evidence record.

    `ref` and `commit` stand in for the tree the collector recorded.
    `None` means "the tree could not be identified", which is what a
    record written before the collector tracked it looks like.

    `dirty` stands in for "the working tree carried uncommitted
    changes".  `None` means "the collector did not say", which the
    summary reads as not known to be clean.
    """
    return {
        "timestamp": "%sT02:00:00+00:00" % day,
        "platform": plat,
        "case": case,
        "status": status,
        "reason": reason,
        "command": None,
        "exit_code": 0 if status == "PASS" else 1,
        "duration_seconds": 1.0,
        "stdout_excerpt": "",
        "stderr_excerpt": "",
        "git_ref": ref,
        "git_commit": commit,
        "git_dirty": dirty,
    }


def gate_unsatisfied(summary, required_days):
    """Return the tracked platforms whose green streak is too short.

    The gate iterates over `PLATFORMS` -- the platforms the window is
    *tracked* for -- rather than over the platforms that happen to have
    evidence.  A platform with no records at all scores zero and is
    therefore unsatisfied, so a missing half of the window can never be
    mistaken for a satisfied one.

    Returns a list of "platform (streak/required)" strings, empty when
    the window is satisfied.
    """
    unsatisfied = []
    for plat in PLATFORMS:
        streak = summary.get(plat, {}).get("consecutive_green_days", 0)
        if streak < required_days:
            unsatisfied.append("%s (%d/%d)" % (plat, streak, required_days))
    return unsatisfied


def _self_test():
    """Pin the streak semantics.

    Every assertion here is about the tool refusing to overstate the
    evidence.  The soak's entire value is that a green window means
    seven consecutive green days, so each case below is a way the tool
    could have lied if the logic were wrong.

    The scratch evidence lives in the system temp directory, never in
    the NimRTC working tree: a self-test that wrote records into the
    repository would corrupt the very evidence it is meant to verify.
    """
    failures = 0

    def check(condition, label):
        nonlocal failures
        if condition:
            print("[OK  ] %s" % label)
        else:
            print("[FAIL] %s" % label)
            failures += 1

    cases = ["d", "e"]

    # 1. Seven calendar-consecutive green days is a streak of seven.
    seven = ["2026-11-%02d" % d for d in range(1, 8)]
    recs = [_record(day, case, "PASS") for day in seven for case in cases]
    info = summarise(recs, cases)["windows"]
    check(info["consecutive_green_days"] == 7,
          "seven consecutive green days give a streak of seven")

    # 2. Green days that are NOT calendar-consecutive do not form a
    #    streak.  This is the case that let five green days spread
    #    across seventeen calendar days read as a five-day streak.
    gapped = ["2026-11-01", "2026-11-05", "2026-11-09",
              "2026-11-13", "2026-11-17"]
    recs = [_record(day, case, "PASS") for day in gapped for case in cases]
    info = summarise(recs, cases)["windows"]
    check(info["consecutive_green_days"] == 1,
          "green days separated by gaps do not form a long streak "
          "(got %d)" % info["consecutive_green_days"])
    check(info["best_streak"] == 1,
          "the best streak over gapped days is one (got %d)" % info["best_streak"])
    check(info["days_without_evidence"] ==
          ["2026-11-02", "2026-11-03", "2026-11-04", "2026-11-06",
           "2026-11-07", "2026-11-08", "2026-11-10", "2026-11-11",
           "2026-11-12", "2026-11-14", "2026-11-15", "2026-11-16"],
          "the days without evidence are reported explicitly")

    # 3. A failing day ends the streak.
    recs = [_record(day, case, "PASS") for day in seven[:3] for case in cases]
    recs += [_record(seven[3], "d", "FAIL", reason="runner exited 1")]
    recs += [_record(day, case, "PASS") for day in seven[4:] for case in cases]
    info = summarise(recs, cases)["windows"]
    check(info["consecutive_green_days"] == 3,
          "a failing day ends the streak (got %d)"
          % info["consecutive_green_days"])
    check(len(info["failures"]) == 1,
          "the failing day is reported as a failure")

    # 4. An inconclusive day is never green and ends the streak.
    recs = [_record(day, case, "PASS") for day in seven[:3] for case in cases]
    recs += [_record(seven[3], "d", "INCONCLUSIVE", reason="no build artefacts")]
    recs += [_record(seven[3], "e", "INCONCLUSIVE", reason="no build artefacts")]
    recs += [_record(day, case, "PASS") for day in seven[4:] for case in cases]
    info = summarise(recs, cases)["windows"]
    check(info["consecutive_green_days"] == 3,
          "an inconclusive day ends the streak (got %d)"
          % info["consecutive_green_days"])
    check(len(info["inconclusive_days"]) == 1,
          "the inconclusive day is reported separately")

    # 5. A day where only one of the required cases passed is not green.
    recs = [_record(day, "d", "PASS") for day in seven]
    recs += [_record(day, "e", "PASS") for day in seven[1:]]
    info = summarise(recs, cases)["windows"]
    check(info["consecutive_green_days"] == 6,
          "a day missing one required case is not green (streak %d, expected 6)"
          % info["consecutive_green_days"])

    # 6. The six-day shortfall must not be reported as seven.
    recs = [_record(day, case, "PASS") for day in seven[:6] for case in cases]
    info = summarise(recs, cases)["windows"]
    check(info["consecutive_green_days"] == 6,
          "six green days report a streak of six, not seven")

    # 7. Platforms are tracked separately: a green Windows day must not
    #    be credited to the Linux half of the gate.
    recs = [_record(day, case, "PASS", plat="windows") for day in seven for case in cases]
    recs += [_record(day, case, "PASS", plat="linux-x86_64")
             for day in seven[:2] for case in cases]
    summary = summarise(recs, cases)
    check(summary["windows"]["consecutive_green_days"] == 7,
          "the Windows streak is unaffected by the Linux records")
    check(summary["linux-x86_64"]["consecutive_green_days"] == 2,
          "the Linux streak is computed from its own records only")

    # 8. A malformed record is skipped, not fatal.
    tmp = Path(tempfile.mkdtemp(prefix="nimrtc-soak-selftest-"))
    try:
        path = tmp / "windows-2026-11-01.jsonl"
        with path.open("w", encoding="utf-8") as handle:
            handle.write(json.dumps(_record("2026-11-01", "d", "PASS")) + "\n")
            handle.write("{ this is not json\n")
            handle.write(json.dumps(_record("2026-11-01", "e", "PASS")) + "\n")
        loaded = load_records(tmp)
        check(len(loaded) == 2,
              "a malformed line is skipped and the rest still load (got %d)"
              % len(loaded))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # 9. The evidence file is written per platform and per day.
    tmp = Path(tempfile.mkdtemp(prefix="nimrtc-soak-selftest-"))
    try:
        stamp = datetime(2026, 11, 1, 2, 0, 0, tzinfo=timezone.utc)
        path = evidence_path(tmp, "windows", stamp)
        check(path.name == "windows-2026-11-01.jsonl",
              "the evidence file is named per platform and per day")
        append_record(path, _record("2026-11-01", "d", "PASS"))
        append_record(path, _record("2026-11-01", "e", "PASS"))
        check(len(load_records(tmp)) == 2,
              "both records land in the same day's evidence file")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # 10. A FAIL followed by a PASS on the same day IS green, and the
    #     failure is still reported.  A day is normally measured more
    #     than once -- the CI job and the scheduled local task both
    #     write the same platform/day, and either may be re-run after a
    #     fix -- so one FAIL among passing runs is evidence about the
    #     runner, not about the program.  The FAIL must still be
    #     reported: it is not erased, it is just no longer fatal.
    recs = [_record(day, case, "PASS") for day in seven[:3] for case in cases]
    recs += [_record(seven[3], "d", "FAIL", reason="runner exited 1")]
    recs += [_record(seven[3], "d", "PASS")]
    recs += [_record(seven[3], "e", "PASS")]
    recs += [_record(day, case, "PASS") for day in seven[4:] for case in cases]
    info = summarise(recs, cases)["windows"]
    check(info["consecutive_green_days"] == 7,
          "a day with a FAIL followed by a PASS is green "
          "(streak %d, expected 7)" % info["consecutive_green_days"])
    check(len(info["failures"]) == 1,
          "the FAIL is still reported even though the day later passed")
    check(not any(entry["day"] == seven[3] for entry in info["not_green_days"]),
          "the day that ended with a passing run is not listed as not green")

    # 10b. A case that failed EVERY time it ran is still fatal, even
    #      when the other case passed.  This is the case the softened
    #      rule must not let through: no passing run exists for that
    #      case, so the day carries no evidence that it works.
    recs = [_record(day, case, "PASS") for day in seven[:3] for case in cases]
    recs += [_record(seven[3], "d", "FAIL", reason="runner exited 1")]
    recs += [_record(seven[3], "d", "FAIL", reason="runner exited 1")]
    recs += [_record(seven[3], "e", "PASS")]
    recs += [_record(day, case, "PASS") for day in seven[4:] for case in cases]
    info = summarise(recs, cases)["windows"]
    check(info["consecutive_green_days"] == 3,
          "a case that failed every time it ran ends the streak "
          "(streak %d, expected 3)" % info["consecutive_green_days"])
    check(len(info["failures"]) == 2,
          "both FAILs are reported (got %d, expected 2)"
          % len(info["failures"]))
    check(info["not_green_days"] and info["not_green_days"][0]["day"] == seven[3],
          "the day whose case never passed is listed as not green")
    check(info["not_green_days"][0]["cases_without_pass"] == ["d"],
          "the day is reported as lacking a PASS for the case that never "
          "passed (got %s)" % info["not_green_days"][0]["cases_without_pass"])

    # 11. An INCONCLUSIVE followed by a PASS for the same case IS green:
    #     running twice is harmless, and the successful run is the
    #     evidence that counts.
    recs = [_record(day, case, "PASS") for day in seven for case in cases]
    recs += [_record(seven[0], "d", "INCONCLUSIVE", reason="no build artefacts")]
    info = summarise(recs, cases)["windows"]
    check(info["consecutive_green_days"] == 7,
          "an INCONCLUSIVE followed by a PASS is still green "
          "(streak %d, expected 7)" % info["consecutive_green_days"])

    # 12. Every failing record is reported, not just the first one.
    recs = [_record(seven[0], "d", "FAIL", reason="runner exited 1")]
    recs += [_record(seven[0], "e", "FAIL", reason="runner exited 2")]
    recs += [_record(seven[1], "d", "FAIL", reason="runner exited 3")]
    info = summarise(recs, cases)["windows"]
    check(len(info["failures"]) == 3,
          "every failing record is reported (got %d, expected 3)"
          % len(info["failures"]))

    # 13. A tracked platform with no evidence at all appears in the
    #     summary with a zero streak, so it cannot be mistaken for a
    #     platform that is simply not being tracked.
    summary = summarise([], cases)
    check("linux-x86_64" in summary,
          "a tracked platform with no evidence still appears in the summary")
    check(summary["linux-x86_64"]["consecutive_green_days"] == 0,
          "a tracked platform with no evidence has a zero streak")
    check(gate_unsatisfied(summary, 7) != [],
          "a tracked platform with no evidence fails the gate")

    # 14. An exactly duplicated record is loaded once.  The merge
    #     procedure appends the CI records into build/soak/, so running
    #     it twice writes the same record twice -- that is not new
    #     evidence and must not inflate the failure count.
    tmp = Path(tempfile.mkdtemp(prefix="nimrtc-soak-selftest-"))
    try:
        path = tmp / "windows-2026-11-01.jsonl"
        record = _record("2026-11-01", "d", "FAIL", reason="runner exited 1")
        with path.open("w", encoding="utf-8") as handle:
            handle.write(json.dumps(record) + "\n")
            handle.write(json.dumps(record) + "\n")
            handle.write(json.dumps(_record("2026-11-01", "e", "PASS")) + "\n")
        loaded = load_records(tmp)
        check(len(loaded) == 2,
              "an exactly duplicated record is loaded once (got %d, expected 2)"
              % len(loaded))
        info = summarise(loaded, cases)["windows"]
        check(len(info["failures"]) == 1,
              "a duplicated failure is reported once (got %d, expected 1)"
              % len(info["failures"]))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # 15. A day measured on a tree other than the candidate is not
    #     green.  The tag is cut from `release/1.1`, so a green day
    #     taken on `main` is evidence about a different program and
    #     must not be credited to the window.
    recs = [_record(day, case, "PASS", ref="release/1.1")
            for day in seven for case in cases]
    info = summarise(recs, cases, expect_ref="release/1.1")["windows"]
    check(info["consecutive_green_days"] == 7,
          "seven days measured on the candidate branch give a streak of seven "
          "(got %d)" % info["consecutive_green_days"])

    recs = [_record(day, case, "PASS", ref="main")
            for day in seven for case in cases]
    info = summarise(recs, cases, expect_ref="release/1.1")["windows"]
    check(info["consecutive_green_days"] == 0,
          "seven green days measured on another tree give a streak of zero "
          "(got %d)" % info["consecutive_green_days"])
    check(len(info["wrong_ref_days"]) == 7,
          "every day measured on another tree is listed as such "
          "(got %d, expected 7)" % len(info["wrong_ref_days"]))
    check(gate_unsatisfied(summarise(recs, cases, expect_ref="release/1.1"), 7) != [],
          "a window measured on the wrong tree fails the gate")

    # 16. A day whose tree could not be identified is not green either:
    #     "unknown" is not a statement about the candidate.
    recs = [_record(day, case, "PASS") for day in seven for case in cases]
    info = summarise(recs, cases, expect_ref="release/1.1")["windows"]
    check(info["consecutive_green_days"] == 0,
          "a day with an unidentified tree is not green "
          "(streak %d, expected 0)" % info["consecutive_green_days"])

    # 17. Without an expectation, the tree does not enter the verdict at
    #     all -- the historical behaviour is preserved for anyone
    #     summarising evidence for a different release.
    recs = [_record(day, case, "PASS", ref="main")
            for day in seven for case in cases]
    info = summarise(recs, cases)["windows"]
    check(info["consecutive_green_days"] == 7,
          "without an expected tree the tree does not affect the streak "
          "(got %d)" % info["consecutive_green_days"])

    # 18. A day measured on a tree that carried uncommitted changes is
    #     not green.  The commit identifies the tree, but a dirty
    #     working tree is not that tree -- the program that ran is
    #     whatever the working directory happened to contain, so the
    #     day says nothing about the tree that gets tagged.
    recs = [_record(day, case, "PASS", ref="release/1.1")
            for day in seven for case in cases]
    recs += [_record(seven[3], "d", "PASS", ref="release/1.1", dirty=True)]
    info = summarise(recs, cases, expect_ref="release/1.1")["windows"]
    check(info["consecutive_green_days"] == 3,
          "a day measured on an unclean tree ends the streak "
          "(streak %d, expected 3)" % info["consecutive_green_days"])
    check(len(info["dirty_tree_days"]) == 1,
          "the day measured on an unclean tree is listed as such "
          "(got %d, expected 1)" % len(info["dirty_tree_days"]))
    check(gate_unsatisfied(summarise(recs, cases, expect_ref="release/1.1"), 7) != [],
          "a window containing an unclean-tree day fails the gate")

    # 19. A record that does not say whether the tree was clean is read
    #     as not known to be clean -- the gate cannot certify what it
    #     cannot see.  This is what a record written before the
    #     collector tracked the flag looks like.
    recs = [_record(day, case, "PASS", ref="release/1.1")
            for day in seven for case in cases]
    for rec in recs:
        rec["git_dirty"] = None
    info = summarise(recs, cases, expect_ref="release/1.1")["windows"]
    check(info["consecutive_green_days"] == 0,
          "a day whose cleanliness is unknown is not green "
          "(streak %d, expected 0)" % info["consecutive_green_days"])
    check(len(info["unknown_cleanliness_days"]) == 7,
          "a day whose cleanliness is unknown is reported as unknown, not as "
          "dirty (got %d, expected 7)" % len(info["unknown_cleanliness_days"]))
    check(not info["dirty_tree_days"],
          "a day whose cleanliness is unknown is not reported as dirty")

    # 20. The ancestry rule: a day measured on a commit that the
    #     candidate DESCENDS FROM is green, even though the branch name
    #     may differ.  This is the rule that makes a window measured on
    #     a moving `main` usable for a tag cut from a later descendant.
    def always_ancestor(commit, candidate):
        return True

    recs = [_record(day, case, "PASS", ref="main", commit="c%d" % i)
            for i, day in enumerate(seven) for case in cases]
    info = summarise(recs, cases, expect_ref="main",
                     expect_ancestor_of="candidate",
                     resolver=always_ancestor)["windows"]
    check(info["consecutive_green_days"] == 7,
          "days measured on ancestors of the candidate are green "
          "(streak %d, expected 7)" % info["consecutive_green_days"])

    # 21. A day measured on a tree the candidate does NOT descend from
    #     proves nothing about the candidate, even when the case passed.
    def never_ancestor(commit, candidate):
        return False

    info = summarise(recs, cases, expect_ref="main",
                     expect_ancestor_of="candidate",
                     resolver=never_ancestor)["windows"]
    check(info["consecutive_green_days"] == 0,
          "days measured on an unrelated tree are not green "
          "(streak %d, expected 0)" % info["consecutive_green_days"])
    check(len(info["unrelated_tree_days"]) == 7,
          "every unrelated-tree day is reported as such "
          "(got %d, expected 7)" % len(info["unrelated_tree_days"]))
    check(gate_unsatisfied(summarise(recs, cases, expect_ref="main",
                                     expect_ancestor_of="candidate",
                                     resolver=never_ancestor), 7) != [],
          "a window measured on an unrelated tree fails the gate")

    # 22. "Cannot tell" is not "yes": a record whose commit cannot be
    #     resolved is not evidence, and the gate must refuse it.
    def unknown_ancestor(commit, candidate):
        return None

    info = summarise(recs, cases, expect_ref="main",
                     expect_ancestor_of="candidate",
                     resolver=unknown_ancestor)["windows"]
    check(info["consecutive_green_days"] == 0,
          "a day whose ancestry cannot be established is not green "
          "(streak %d, expected 0)" % info["consecutive_green_days"])
    check(len(info["undecidable_tree_days"]) == 7,
          "an undecidable day is reported as undecidable, not as unrelated "
          "(got %d, expected 7)" % len(info["undecidable_tree_days"]))
    check(gate_unsatisfied(summarise(recs, cases, expect_ref="main",
                                     expect_ancestor_of="candidate",
                                     resolver=unknown_ancestor), 7) != [],
          "a window whose ancestry cannot be established fails the gate")

    # 23. The two rules compose: a day that satisfies the ancestry rule
    #     but was measured on ANOTHER branch is still not green, because
    #     the release gate asks for both.
    info = summarise(recs, cases, expect_ref="release/1.1",
                     expect_ancestor_of="candidate",
                     resolver=always_ancestor)["windows"]
    check(info["consecutive_green_days"] == 0,
          "the ancestry rule does not override the branch rule "
          "(streak %d, expected 0)" % info["consecutive_green_days"])

    # 24. A record with no commit at all cannot satisfy the ancestry
    #     rule: the tree cannot be placed in the candidate's history.
    recs = [_record(day, case, "PASS", ref="main") for day in seven
            for case in cases]
    info = summarise(recs, cases, expect_ref="main",
                     expect_ancestor_of="candidate",
                     resolver=always_ancestor)["windows"]
    check(info["consecutive_green_days"] == 0,
          "a day with no recorded commit is not green under the ancestry rule "
          "(streak %d, expected 0)" % info["consecutive_green_days"])

    # 25. The freshness rule: a seven-day streak that closed long
    #     before the candidate is not evidence about it.  This is the
    #     rule that stops a window measured two generations ago from
    #     satisfying today's release.
    recent = ["2026-11-%02d" % d for d in range(1, 8)]
    recs = [_record(day, case, "PASS", plat="windows", ref="main")
            for day in recent for case in cases]
    recs += [_record(day, case, "PASS", plat="linux-x86_64", ref="main")
             for day in recent for case in cases]
    fresh_summary = summarise(recs, cases, expect_ref="main")
    check(stale_platforms(fresh_summary, "2026-11-08", 30) == [],
          "a window that closed within the limit is not stale")
    check(stale_platforms(fresh_summary, "2026-12-20", 30) != [],
          "a window that closed long before the candidate is stale")
    check(stale_platforms(fresh_summary, "2026-12-20", 365) == [],
          "a window within a year of the candidate is not stale")

    # 26. A platform with no green day is not reported as stale: it is
    #     already unsatisfied by the streak rule, and reporting both
    #     reasons would confuse the two.
    empty_summary = summarise([], cases, expect_ref="main")
    check(stale_platforms(empty_summary, "2026-11-20", 30) == [],
          "a platform with no green day is not reported as stale")

    print()
    print("self-test: %d failure(s)" % failures)
    return 0 if failures == 0 else 1


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Collect / summarise the NimRTC 7-day soak evidence.")
    parser.add_argument("--platform", default=None, choices=list(PLATFORMS),
                        help="platform to run for (default: this machine's)")
    parser.add_argument("--case", default="d,e",
                        help="comma-separated acceptance cases (default: d,e)")
    parser.add_argument("--out-dir", default=None, type=Path,
                        help="evidence directory (default: build/soak)")
    parser.add_argument("--runner", default=None, type=Path,
                        help="acceptance runner (default: tools/run_e2e_acceptance.py)")
    parser.add_argument("--summary", action="store_true",
                        help="summarise the evidence instead of running a case")
    parser.add_argument("--json", action="store_true",
                        help="emit machine-readable output")
    parser.add_argument("--require", action="store_true",
                        help="make --summary a gate: exit non-zero unless every "
                             "tracked platform has a streak of at least "
                             "--require-days consecutive green days")
    parser.add_argument("--require-days", type=int, default=5,
                        help="number of consecutive green days the gate demands "
                             "(default: 5)")
    parser.add_argument("--expect-ref", default=DEFAULT_EXPECT_REF,
                        help="the tree the window is a statement about; a day "
                             "measured on any other tree is not green "
                             "(default: %s; pass an empty string to disable)"
                             % DEFAULT_EXPECT_REF)
    parser.add_argument("--expect-ancestor-of", default=None,
                        help="a commit-ish (a SHA, a tag, a branch) that the "
                             "measured tree must be an ancestor of; a day "
                             "measured on a tree the candidate does not "
                             "descend from is not green.  This is the rule "
                             "a release cut from a moving branch needs: the "
                             "window is measured on `main`, and the tag is "
                             "cut from a descendant of every commit that was "
                             "measured")
    parser.add_argument("--self-test", action="store_true",
                        help="run the built-in self-test and exit")
    parser.add_argument("--max-window-age-days", type=int, default=None,
                        help="the window must have closed within this many "
                             "days of the candidate commit; a seven-day "
                             "streak from six months ago is not evidence "
                             "about today's release.  Disabled when not "
                             "given.")
    args = parser.parse_args(argv)

    if args.self_test:
        return _self_test()

    out_dir = (args.out_dir or DEFAULT_OUT_DIR).resolve()
    runner = (args.runner or DEFAULT_RUNNER).resolve()
    required_cases = [case.strip() for case in args.case.split(",") if case.strip()]

    if args.summary:
        records = load_records(out_dir)
        # The ancestry rule is stated in terms of a commit, so a
        # branch-or-tag given on the command line is resolved here.
        # An unresolvable ref is NOT silently ignored: it becomes an
        # explicit failure, because a gate that silently drops its own
        # strongest rule is worse than one that refuses to run.
        ancestor_commit = None
        if args.expect_ancestor_of:
            ancestor_commit = resolve_commitish(args.expect_ancestor_of)
            if ancestor_commit is None:
                sys.stderr.write(
                    "error: cannot resolve --expect-ancestor-of %r to a "
                    "commit; the window's ancestry rule cannot be applied, so "
                    "the summary would not mean what it claims to\n"
                    % args.expect_ancestor_of)
                return 2
        summary = summarise(records, required_cases, expect_ref=args.expect_ref,
                            expect_ancestor_of=ancestor_commit)
        if args.json:
            print(json.dumps({
                "out_dir": str(out_dir),
                "cases": required_cases,
                "expect_ref": args.expect_ref,
                "expect_ancestor_of": args.expect_ancestor_of,
                "expect_ancestor_commit": ancestor_commit,
                "records": len(records),
                "platforms": summary,
            }, indent=2, sort_keys=True))
        else:
            print_summary(summary, required_cases, expect_ref=args.expect_ref,
                          expect_ancestor_of=ancestor_commit)
            if not summary:
                print("no evidence found under %s" % out_dir)
        # The summary is a report by default: it exits 0 even when the
        # window is unsatisfied, so it can be used as a status display.
        #
        # `--require` turns it into a gate: it exits 1 unless EVERY
        # tracked platform has a streak of at least `--require-days`
        # consecutive green days.  That distinction matters, because a
        # summary that always exits 0 cannot be wired into a gate
        # script without someone reading the numbers by hand -- and
        # reading them by hand is exactly how a window gets claimed
        # that was never satisfied.
        if args.require:
            required_days = args.require_days
            unsatisfied = gate_unsatisfied(summary, required_days)
            if unsatisfied:
                sys.stderr.write(
                    "window NOT satisfied: %s\n"
                    % ", ".join(unsatisfied))
                return 1
            # The freshness rule.  A seven-day streak says the window was
            # green; it does not say WHEN.  A window that closed six
            # months ago is not evidence about a release cut today, so
            # the gate also asks how long ago the window closed.
            if args.max_window_age_days is not None:
                reference = (commit_date(ancestor_commit) if ancestor_commit
                             else datetime.now(timezone.utc).strftime("%Y-%m-%d"))
                if reference is None:
                    sys.stderr.write(
                        "error: cannot establish the candidate commit's date, "
                        "so the window's freshness cannot be checked; the gate "
                        "would not mean what it claims to\n")
                    return 1
                stale = stale_platforms(summary, reference,
                                         args.max_window_age_days)
                if stale:
                    sys.stderr.write(
                        "window is TOO OLD: %s\n" % ", ".join(stale))
                    return 1
                print("window closed within %d day(s) of the candidate (%s)"
                      % (args.max_window_age_days, reference))
            print("window satisfied: %d consecutive green days on %s"
                  % (required_days, ", ".join(PLATFORMS)))
        return 0

    plat = args.platform or local_platform()
    if plat not in PLATFORMS:
        sys.stderr.write("warning: platform '%s' is not one of the tracked "
                         "platforms %s; recording under it anyway\n"
                         % (plat, ", ".join(PLATFORMS)))

    failures = 0
    for case in required_cases:
        record = run_once(runner, plat, case, out_dir)
        status = record["status"]
        print("[%-12s] %s / %s" % (status, plat, case))
        if record.get("reason"):
            print("             %s" % record["reason"])
        if status == "FAIL":
            failures += 1
        if record.get("stderr_excerpt"):
            for line in record["stderr_excerpt"].splitlines()[-5:]:
                print("             | %s" % line)

    print()
    print("evidence written to %s" % out_dir)
    return 1 if failures else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except ToolError as exc:
        sys.stderr.write("ERROR: %s\n" % exc)
        sys.exit(2)
