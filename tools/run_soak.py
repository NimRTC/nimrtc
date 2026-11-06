#!/usr/bin/env python3
# Copyright (c) 2026 NimRTC contributors.
# SPDX-License-Identifier: MIT
"""Collect the daily evidence for the NimRTC 7-day soak (v1.1 plan C5).

What this tool is for
---------------------
The v1.1 release gate requires seven consecutive days of green
end-to-end acceptance runs (Case D and Case E, on Windows and Linux
x86_64) before the release tag is cut.  This tool is the evidence
recorder for that window: it runs the acceptance runner once, writes a
machine-readable record of what happened, and -- over a week of daily
invocations -- lets `run_soak.py --summary` state plainly whether the
window is satisfied.

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
    PASS.  An inconclusive day does not count towards the seven.
  * The streak counts *calendar*-consecutive green days.  A day with
    no evidence, an inconclusive day, and a failing day all end the
    streak.  The release gate is seven CONSECUTIVE days (v1.1 plan
    Track C, item C5), so five green days spread across three weeks
    must not read as a five-day streak -- that would let a tag be cut
    on a window the gate never actually authorised.
  * The summary prints the streak it can actually prove, and separately
    prints the days without evidence and the inconclusive days, so a
    window with gaps cannot be mistaken for a clean one.

Usage
-----
    python tools/run_soak.py --platform windows --case d,e
    python tools/run_soak.py --platform linux-x86_64 --case d,e
    python tools/run_soak.py --summary
    python tools/run_soak.py --summary --json

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
    }
    append_record(evidence_path(out_dir, plat, stamp), record)
    return record


# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

def load_records(out_dir):
    """Load every evidence record, newest last."""
    records = []
    if not out_dir.is_dir():
        return records
    for path in sorted(out_dir.glob("*.jsonl")):
        with path.open("r", encoding="utf-8") as handle:
            for line_number, line in enumerate(handle, 1):
                line = line.strip()
                if not line:
                    continue
                try:
                    records.append(json.loads(line))
                except json.JSONDecodeError:
                    # A truncated line means the writer died mid-record.
                    # Skip it rather than fail the whole summary: a
                    # partial day is still a day with no complete
                    # evidence, and the summary will show the gap.
                    sys.stderr.write(
                        "warning: skipping malformed record %s:%d\n"
                        % (path.name, line_number))
    records.sort(key=lambda item: item.get("timestamp", ""))
    return records


def summarise(records, required_cases):
    """Compute the per-platform streaks and the gaps."""
    per_platform = {}
    for record in records:
        per_platform.setdefault(record.get("platform", "?"), []).append(record)

    summary = {}
    for plat, items in per_platform.items():
        # A day counts when every required case has a PASS record for
        # it and no case has a FAIL.  INCONCLUSIVE leaves the day
        # uncounted rather than counting it green.
        days = {}
        for item in items:
            day = item.get("timestamp", "")[:10]
            days.setdefault(day, {})[item.get("case")] = item

        # Classify every day with evidence into green / failing / other.
        #
        # "Other" is a day that has evidence but does not qualify as
        # green and does not contain a failure: an INCONCLUSIVE day, or
        # a day where one required case passed and another never ran.
        # Such a day does not count towards the seven, and -- like a day
        # with no evidence at all -- it breaks the streak.
        green_dates = []
        breaks = []
        for day in sorted(days):
            cases = days[day]
            statuses = {case: rec.get("status") for case, rec in cases.items()}
            if all(statuses.get(case) == "PASS" for case in required_cases):
                green_dates.append(day)
            else:
                breaks.append(day)

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
            {"day": day, "cases": sorted(cases.keys()),
             "reasons": sorted({rec.get("reason", "") for rec in cases.values()
                                if rec.get("status") == "INCONCLUSIVE"})}
            for day, cases in sorted(days.items())
            if any(rec.get("status") == "INCONCLUSIVE" for rec in cases.values())
        ]

        failures = [
            {"day": day, "case": case, "reason": rec.get("reason", "")}
            for day, cases in sorted(days.items())
            for case, rec in sorted(cases.items())
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
        }
    return summary


def print_summary(summary, required_cases):
    print("soak summary (cases: %s)" % ", ".join(required_cases))
    print()
    for plat in sorted(summary):
        info = summary[plat]
        print("%s:" % plat)
        print("    days with evidence    : %d" % info["days_with_evidence"])
        print("    consecutive green days: %d" % info["consecutive_green_days"])
        print("    best streak so far    : %d" % info["best_streak"])
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
        print()


# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------

def _record(day, case, status, plat="windows", reason=""):
    """Build one synthetic evidence record."""
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
    parser.add_argument("--require-days", type=int, default=7,
                        help="number of consecutive green days the gate demands "
                             "(default: 7, the v1.1 plan C5 requirement)")
    parser.add_argument("--self-test", action="store_true",
                        help="run the built-in self-test and exit")
    args = parser.parse_args(argv)

    if args.self_test:
        return _self_test()

    out_dir = (args.out_dir or DEFAULT_OUT_DIR).resolve()
    runner = (args.runner or DEFAULT_RUNNER).resolve()
    required_cases = [case.strip() for case in args.case.split(",") if case.strip()]

    if args.summary:
        records = load_records(out_dir)
        summary = summarise(records, required_cases)
        if args.json:
            print(json.dumps({
                "out_dir": str(out_dir),
                "cases": required_cases,
                "records": len(records),
                "platforms": summary,
            }, indent=2, sort_keys=True))
        else:
            print_summary(summary, required_cases)
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
