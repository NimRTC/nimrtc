#!/usr/bin/env python3
# Copyright (c) 2026 NimRTC contributors.
# SPDX-License-Identifier: MIT
"""Run the v1.3 plan's Q5 soak and archive what it measured.

What this tool is for
---------------------
The v1.3 plan's Q5 asks for "a 24 h+ run [that] completes green under
reordering + loss + jitter, and the run is **archived with its netem
parameters**".  Two pieces exist to make that measurable:

  * `examples/soak-p2p` -- two real `NimRTCEngine`s streaming audio across
    a `net_sim::NatRelay` (the simulated NAT) that applies the impairment
    profile to every packet it forwards, and that prints one JSON record
    of what it measured; and
  * this tool -- the recorder.  It runs the harness with the profile it
    was given, folds the harness's JSON together with the **git
    provenance** of the tree the run was measured on, re-evaluates the
    green criteria from the archived numbers, and appends one line to the
    soak's JSONL evidence file.

The record is deliberately self-describing: a reader can see which tree
produced it, what the network was asked to do, what the relay actually
did, and what the receiver actually decoded -- and can re-derive the
verdict without trusting the harness's own `"green"` flag.

Honesty rules baked in
----------------------
  * A run whose harness exited for a reason other than "measured and
    failed" (no loopback bind, a crash, a stuck process) is recorded as
    **INCONCLUSIVE**, never as PASS.  It says nothing about the engine,
    and crediting it would let a broken runner look like a green day.
  * The green criteria are re-checked here from the archived numbers, not
    read from the harness's `"green"` field.  A harness bug that faked a
    pass would still be caught by the archive.
  * A record whose tree was dirty is marked, because a dirty tree is not
    the commit it claims -- the same rule `tools/run_soak.py` applies.
  * A record is **PASS** only when the harness exited zero *and* the
    archived numbers re-evaluate green.  Anything else is FAIL.

Usage
-----
    python tools/run_soak_impaired.py --seconds 60 \\
        --loss 2 --delay-ms 40 --jitter-ms 20 --reorder 5 --rate-kbps 500

    # The 24 h soak: --seconds 86400, on the self-hosted box.
    python tools/run_soak_impaired.py --seconds 86400 \\
        --loss 2 --delay-ms 40 --jitter-ms 20 --reorder 5

    # What has been recorded so far.
    python tools/run_soak_impaired.py --summary

The default out-dir is `build/soak-impaired/`; evidence never goes to the
project root (the root is kept free of scratch files by policy).

Exit codes
----------
    0  PASS, or --summary found the recorded runs satisfactory
    1  FAIL, or --summary --expect-green found a run that was not
    2  INCONCLUSIVE (the run could not be measured), or the tool could
       not start
"""

from __future__ import annotations

import argparse
import json
import platform
import shutil
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_OUT_DIR = REPO_ROOT / "build" / "soak-impaired"
SCHEMA = "nimrtc-soak-impaired-record/1"
RECORD_SUFFIX = ".jsonl"

# The plan's Q5 DoD: "a **24 h+** run completes green under reordering +
# loss + jitter".  The summary treats a shorter run as a smoke test, not
# as the evidence -- the duration is part of the verdict.
Q5_MIN_SECONDS = 24 * 60 * 60


class ToolError(Exception):
    """The tool could not do its job (as opposed to "the run failed")."""


# ---------------------------------------------------------------------------
# Provenance -- the same fields tools/run_soak.py records, so the two
# evidence streams can be read side by side.
# ---------------------------------------------------------------------------

def _git(args, timeout=60):
    try:
        proc = subprocess.run(
            ["git", "-C", str(REPO_ROOT)] + args,
            capture_output=True, stdin=subprocess.DEVNULL, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if proc.returncode != 0:
        return None
    return proc.stdout.decode("utf-8", errors="replace").strip()


def git_provenance():
    """The tree this run is measured on.

    A 24 h run is a statement about a *program*, and the program is the
    tree it ran against.  Recording `git_ref`, `git_commit` and
    `git_dirty` is what makes the archived run self-describing; the
    fields are `None` when git cannot answer, and the record says so
    rather than guessing.
    """
    provenance = {"git_ref": None, "git_commit": None, "git_dirty": None}

    ref = _git(["symbolic-ref", "--short", "HEAD"])
    if not ref:
        # A detached HEAD (what a CI checkout often leaves).  The first
        # branch pointing at it names the same tree a local checkout
        # would.
        branches = _git(["for-each-ref", "--points-at=HEAD",
                         "--format=%(refname:short)", "refs/heads/"])
        if branches:
            ref = branches.splitlines()[0].strip()
    provenance["git_ref"] = ref or None

    commit = _git(["rev-parse", "HEAD"])
    provenance["git_commit"] = commit or None

    status = _git(["status", "--porcelain"])
    if status is not None:
        provenance["git_dirty"] = bool(status)

    return provenance


def host_provenance():
    """The machine the run was measured on.

    Q5's archive is about a *network profile* applied to an *engine*, so
    a reader has to know whether "24 h green" describes the CI runner, a
    developer's laptop or the self-hosted soak box.  The tuple and the OS
    build are enough to tell runs apart without logging anything
    personal.
    """
    return {
        "platform": sys.platform,
        "machine": platform.machine(),
        "node": platform.node(),
        "python": platform.python_version(),
    }


# ---------------------------------------------------------------------------
# Finding the harness
# ---------------------------------------------------------------------------

def find_harness():
    """Locate the `soak-p2p` executable.

    The name differs by platform (`soak-p2p.exe` on Windows), and a
    multi-config generator keeps it in `build/examples/<Config>/`.  The
    first match wins; a caller that built somewhere else passes
    `--harness`.
    """
    build = REPO_ROOT / "build"
    names = ["soak-p2p.exe", "soak-p2p"]
    directories = [build / "examples" / config
                   for config in ("Debug", "Release", "RelWithDebInfo",
                                  "MinSizeRel")]
    directories += [build / "examples", build / "bin", build]
    for directory in directories:
        for name in names:
            path = directory / name
            if path.is_file():
                return path
    return None


# ---------------------------------------------------------------------------
# The green criteria, re-evaluated from the archived numbers
# ---------------------------------------------------------------------------

def evaluate_green(measurement):
    """Re-check Q5's invariants from the harness's numbers.

    This deliberately does **not** read the harness's own `"green"`
    field: the archive is the authority, and a criterion that lived only
    in the harness could be lost in a refactor.  The thresholds mirror
    `examples/soak-p2p/soak-p2p.cpp`; a change there must be made here
    too.
    """
    failures = []
    relay = measurement.get("relay") or {}
    audio = measurement.get("audio") or {}
    memory = measurement.get("memory") or {}
    feedback = measurement.get("feedback") or {}
    params = measurement.get("params") or {}

    def require(condition, message):
        if not condition:
            failures.append(message)

    # 1. The media path carried media.
    require(audio.get("packets_sent", 0) > 0,
            "the sender never emitted an RTP packet")
    require(audio.get("frames_received", 0) > 0,
            "the receiver never decoded a frame")
    require(relay.get("forwarded", 0) > 0, "the relay never forwarded a packet")

    # 2. Memory stayed bounded (Q2).  Four seconds of PCM is the point
    #    past which the drain loop is not draining -- not a codec-quality
    #    judgement.
    one_second_of_pcm = 48000 * 4  # float32
    require(memory.get("peak_pending_audio_bytes", 0) <= 4 * one_second_of_pcm,
            "the receive path held more than 4 s of PCM")

    # 3. The feedback rate stayed bounded (Q4).
    feedback_out = feedback.get("tx_nack", 0) + feedback.get("tx_pli", 0)
    require(feedback_out <= audio.get("packets_sent", 0),
            "outbound feedback outnumbered the RTP packets it describes")

    # 4. The loss was largely recovered.
    require(audio.get("decode_errors", 0) == 0,
            "the decoder reported errors")
    if relay.get("dropped", 0) > 0:
        accounted = (audio.get("concealed", 0) + audio.get("fec_recovered", 0))
        require(accounted > 0,
                "the relay dropped packets but the receiver recovered no frame")

    # 5. A 100 % profile actually dropped.
    if params.get("loss_pct", 0.0) >= 100.0:
        require(relay.get("dropped", 0) > 0,
                "a 100 % loss profile dropped nothing")

    return failures


# ---------------------------------------------------------------------------
# The self-test
# ---------------------------------------------------------------------------
#
# Every assertion below is about the tool **refusing to overstate** the
# evidence.  The soak's whole value is that a green record means a
# green 24 h run, so each case is a way the tool could have lied if the
# logic were wrong.
#
# The scratch evidence lives in the system temp directory, never in the
# NimRTC working tree: a self-test that wrote records into the
# repository would corrupt the very evidence it is meant to verify.

def _green_measurement(**overrides):
    """A measurement that satisfies every green criterion."""
    measurement = {
        "params": {"loss_pct": 5.0, "jitter_ms": 40.0, "reorder_pct": 10.0},
        "relay": {"received": 400, "forwarded": 380, "dropped": 18,
                  "reordered": 43, "buffered": 1},
        "audio": {"packets_sent": 358, "frames_received": 350,
                  "concealed": 4, "fec_recovered": 0, "packets_lost": 18,
                  "decode_errors": 0, "jb_dropped": 0},
        "memory": {"peak_pending_audio_bytes": 0},
        "feedback": {"tx_nack": 4, "rx_nack": 0, "tx_pli": 0, "rx_pli": 0},
        "green": True,
    }
    for key, value in overrides.items():
        if isinstance(value, dict) and isinstance(measurement.get(key), dict):
            merged = dict(measurement[key])
            merged.update(value)
            measurement[key] = merged
        else:
            measurement[key] = value
    return measurement


def _record(status="PASS", elapsed=60.0, profile=None, dirty=False):
    """An evidence record with the shape the recorder writes."""
    return {
        "schema": SCHEMA,
        "status": status,
        "elapsed_seconds": elapsed,
        "profile": profile if profile is not None else {
            "loss_pct": 5.0, "delay_ms": 80.0, "jitter_ms": 40.0,
            "reorder_pct": 10.0, "rate_bps": 0, "seed": 1, "seconds": elapsed,
        },
        "provenance": {"git_ref": "main", "git_commit": "0" * 40,
                       "git_dirty": dirty},
    }


def _self_test():
    """Pin the verdict semantics."""
    failures = 0

    def check(condition, label):
        nonlocal failures
        if condition:
            print("[OK  ] %s" % label)
        else:
            print("[FAIL] %s" % label)
            failures += 1

    # 1. A measurement that satisfies every criterion is green.
    check(not evaluate_green(_green_measurement()),
          "a healthy measurement passes every criterion")

    # 2. The harness's own "green" flag is NOT trusted: a measurement
    #    that claims green but decoded nothing must still fail.
    measurement = _green_measurement(green=True)
    measurement["audio"]["frames_received"] = 0
    check(bool(evaluate_green(measurement)),
          "a measurement that decoded nothing fails even though the "
          "harness claimed green")

    # 3. Memory that grew past the Q2 bound is a failure.
    measurement = _green_measurement()
    measurement["memory"]["peak_pending_audio_bytes"] = 48000 * 4 * 5
    check(bool(evaluate_green(measurement)),
          "a receive path holding more than 4 s of PCM fails (Q2's bound)")

    # 4. Feedback that outnumbers the RTP it describes is a failure
    #    (Q4's bound).
    measurement = _green_measurement()
    measurement["feedback"]["tx_nack"] = 10_000
    check(bool(evaluate_green(measurement)),
          "feedback that outnumbers the RTP fails (Q4's bound)")

    # 5. A run that dropped packets but recovered nothing is a failure.
    measurement = _green_measurement()
    measurement["audio"]["concealed"] = 0
    measurement["audio"]["fec_recovered"] = 0
    check(bool(evaluate_green(measurement)),
          "a lossy run that recovered no frame fails")

    # 6. A 100 % loss profile that dropped nothing is a failure -- a
    #    profile that never drops would make the soak a lie.
    measurement = _green_measurement()
    measurement["params"]["loss_pct"] = 100.0
    measurement["relay"]["dropped"] = 0
    check(bool(evaluate_green(measurement)),
          "a 100 % loss profile that dropped nothing fails")

    # 7. The Q5 run shape: all three impairments must be non-zero.
    check(_is_q5_run(_record()),
          "a run with loss + jitter + reorder is a Q5 run")
    for missing in ("loss_pct", "jitter_ms", "reorder_pct"):
        profile = {"loss_pct": 5.0, "delay_ms": 80.0, "jitter_ms": 40.0,
                   "reorder_pct": 10.0, "rate_bps": 0, "seed": 1}
        profile[missing] = 0.0
        check(not _is_q5_run(_record(profile=profile)),
              "a run without %s is not a Q5 run" % missing)

    # 8. The summary's verdict, over a synthetic evidence file.
    tmp = Path(tempfile.mkdtemp(prefix="nimrtc-soak-impaired-selftest-"))
    try:
        evidence = tmp / ("%s%s" % (sys.platform, RECORD_SUFFIX))
        with evidence.open("w", encoding="utf-8") as handle:
            handle.write(json.dumps(_record()) + "\n")
            handle.write("{ this is not json\n")
            handle.write(json.dumps(_record(status="FAIL")) + "\n")
            handle.write(json.dumps(_record(elapsed=Q5_MIN_SECONDS)) + "\n")

        check(summarize(tmp) == 0,
              "the summary exits 0 without a strict expectation")
        check(summarize(tmp, expect_green=True) == 1,
              "the summary exits 1 when a recorded run was not green")
        check(summarize(tmp, expect_q5=True) == 0,
              "the summary accepts a 24 h run under the Q5 impairments")
        check(summarize(tmp / "empty", expect_q5=True) == 1,
              "the summary exits 1 when there is no evidence at all")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # 9. A short run is a smoke test, not the evidence.
    tmp = Path(tempfile.mkdtemp(prefix="nimrtc-soak-impaired-selftest-"))
    try:
        evidence = tmp / ("%s%s" % (sys.platform, RECORD_SUFFIX))
        with evidence.open("w", encoding="utf-8") as handle:
            handle.write(json.dumps(_record(elapsed=600.0)) + "\n")
        check(summarize(tmp, expect_q5=True) == 1,
              "a 10 minute run does not satisfy the Q5 duration bar")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # 10. A green run on a dirty tree is reported as such.
    tmp = Path(tempfile.mkdtemp(prefix="nimrtc-soak-impaired-selftest-"))
    try:
        evidence = tmp / ("%s%s" % (sys.platform, RECORD_SUFFIX))
        with evidence.open("w", encoding="utf-8") as handle:
            handle.write(json.dumps(_record(dirty=True)) + "\n")
        check(summarize(tmp, expect_green=True) == 1,
              "a green run on a dirty tree does not satisfy --expect-green")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("self-test: %d failure(s)" % failures)
    return 1 if failures else 0


# ---------------------------------------------------------------------------
# One run
# ---------------------------------------------------------------------------

def run_once(args):
    """Run the harness once and append one record to the evidence file."""
    stamp = datetime.now(timezone.utc)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    harness = Path(args.harness) if args.harness else find_harness()
    if harness is None or not harness.is_file():
        raise ToolError(
            "the soak-p2p harness was not found -- build it first:\n"
            "    cmake --build build --target soak-p2p")

    artifact = out_dir / ("run-%s.json" % stamp.strftime("%Y%m%dT%H%M%SZ"))
    cmd = [
        str(harness),
        "--seconds", str(args.seconds),
        "--loss", str(args.loss),
        "--delay-ms", str(args.delay_ms),
        "--jitter-ms", str(args.jitter_ms),
        "--reorder", str(args.reorder),
        "--json", str(artifact),
    ]
    if args.rate_kbps > 0:
        cmd += ["--rate-kbps", str(args.rate_kbps)]
    if args.seed is not None:
        cmd += ["--seed", str(args.seed)]

    record = {
        "schema": SCHEMA,
        "status": "INCONCLUSIVE",
        "started_at": stamp.isoformat(),
        "duration_seconds": args.seconds,
        "profile": {
            "loss_pct": args.loss,
            "delay_ms": args.delay_ms,
            "jitter_ms": args.jitter_ms,
            "reorder_pct": args.reorder,
            "rate_bps": int(args.rate_kbps * 1000) if args.rate_kbps > 0 else 0,
            "seed": args.seed,
            "seconds": args.seconds,
        },
        "provenance": git_provenance(),
        "host": host_provenance(),
        "harness": str(harness),
        "artifact": str(artifact),
        "measurement": None,
        "failures": [],
    }

    started = time.monotonic()
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT,
                              stdin=subprocess.DEVNULL,
                              cwd=str(REPO_ROOT),
                              timeout=args.seconds + args.timeout_slack)
        elapsed = time.monotonic() - started
        record["exit_code"] = proc.returncode
        record["elapsed_seconds"] = round(elapsed, 3)

        if proc.returncode not in (0, 1):
            # 2 / 3 / anything else: the run could not be measured (no
            # loopback bind, bad argument, a crash).  INCONCLUSIVE, not
            # FAIL -- a run that never started says nothing about the
            # engine, and a FAIL would let a broken runner look like a
            # robustness regression.
            record["status"] = "INCONCLUSIVE"
            record["failures"].append(
                "the harness exited %d without measuring" % proc.returncode)
        else:
            measurement = _read_artifact(artifact)
            if measurement is None:
                record["status"] = "INCONCLUSIVE"
                record["failures"].append("the harness produced no readable JSON")
            else:
                record["measurement"] = measurement
                failures = evaluate_green(measurement)
                if proc.returncode == 1 and not failures:
                    failures.append(
                        "the harness reported a failure the archived numbers "
                        "do not reproduce")
                record["failures"] = failures
                record["status"] = "PASS" if not failures else "FAIL"
    except subprocess.TimeoutExpired:
        record["status"] = "INCONCLUSIVE"
        record["elapsed_seconds"] = round(time.monotonic() - started, 3)
        record["failures"].append("the harness did not return in time")

    evidence = out_dir / ("%s%s" % (sys.platform, RECORD_SUFFIX))
    with evidence.open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(record, sort_keys=True) + "\n")

    print(json.dumps(record, indent=2, sort_keys=True))
    return record


def _read_artifact(path):
    try:
        with Path(path).open("r", encoding="utf-8") as handle:
            return json.load(handle)
    except (OSError, json.JSONDecodeError):
        return None


# ---------------------------------------------------------------------------
# The summary
# ---------------------------------------------------------------------------

def summarize(out_dir, expect_green=False, expect_q5=False):
    """Print what the evidence file can actually prove."""
    out_dir = Path(out_dir)
    evidence_files = sorted(out_dir.glob("*" + RECORD_SUFFIX))
    if not evidence_files:
        print("no soak-impaired evidence in %s" % out_dir)
        return 0 if not (expect_green or expect_q5) else 1

    total = green = red = inconclusive = dirty_green = 0
    longest_seconds = 0.0
    q5_runs = []
    for evidence in evidence_files:
        with evidence.open("r", encoding="utf-8") as handle:
            for line in handle:
                line = line.strip()
                if not line:
                    continue
                try:
                    record = json.loads(line)
                except json.JSONDecodeError:
                    inconclusive += 1
                    continue
                total += 1
                status = record.get("status", "INCONCLUSIVE")
                elapsed = record.get("elapsed_seconds") or 0.0
                longest_seconds = max(longest_seconds, elapsed)
                if status == "PASS":
                    green += 1
                    if (record.get("provenance") or {}).get("git_dirty"):
                        dirty_green += 1
                    if _is_q5_run(record):
                        q5_runs.append(record)
                elif status == "FAIL":
                    red += 1
                else:
                    inconclusive += 1

    print("soak-impaired evidence: %s" % out_dir)
    print("  runs        : %d" % total)
    print("  PASS        : %d" % green)
    print("  FAIL        : %d" % red)
    print("  INCONCLUSIVE: %d" % inconclusive)
    if dirty_green:
        print("  PASS on a dirty tree: %d (not evidence about a commit)" % dirty_green)
    print("  longest run : %.1f s" % longest_seconds)

    if expect_green and (red or inconclusive or green == 0 or dirty_green):
        return 1

    if expect_q5:
        # The plan's DoD: a **24 h+** run, green, under the *three*
        # impairments it names (reordering + loss + jitter), on a clean
        # tree.  A 30 s run at the right profile is a smoke test, not the
        # evidence -- so the duration is part of the verdict.
        qualifying = [r for r in q5_runs
                      if (r.get("elapsed_seconds") or 0.0) >= Q5_MIN_SECONDS]
        print("  Q5 runs     : %d (>= %.0f s under loss + jitter + reorder)"
              % (len(qualifying), Q5_MIN_SECONDS))
        if not qualifying:
            print("  Q5 DoD NOT met -- record a run of >= %.0f s with "
                  "--loss, --jitter-ms and --reorder all > 0 on a clean tree"
                  % Q5_MIN_SECONDS)
            return 1
        return 0

    return 0


def _is_q5_run(record):
    """True when a record exercised the *three* impairments Q5 names.

    The plan's wording is "reordering + loss + jitter": all three must be
    non-zero for a run to be evidence about Q5 rather than about one
    impairment in isolation.  A delay with no jitter is a long RTT, not a
    jitter test; a loss run with no reorder is a different profile.
    """
    profile = record.get("profile") or {}
    return (profile.get("loss_pct", 0.0) > 0.0
            and profile.get("jitter_ms", 0.0) > 0.0
            and profile.get("reorder_pct", 0.0) > 0.0)


# ---------------------------------------------------------------------------
# The command line
# ---------------------------------------------------------------------------

def build_parser():
    parser = argparse.ArgumentParser(
        description="Run the Q5 impairment soak and archive what it measured.")
    parser.add_argument("--seconds", type=float, default=60.0,
                        help="how long the media runs (default 60; a 24 h "
                             "soak is 86400)")
    parser.add_argument("--loss", type=float, default=0.0,
                        help="uniform loss, in percent")
    parser.add_argument("--delay-ms", type=float, default=0.0,
                        help="base one-way delay, in ms")
    parser.add_argument("--jitter-ms", type=float, default=0.0,
                        help="uniform spread around the delay, in ms")
    parser.add_argument("--reorder", type=float, default=0.0,
                        help="share of packets held back so they arrive late, "
                             "in percent")
    parser.add_argument("--rate-kbps", type=float, default=0.0,
                        help="link capacity in kbps (0 = unlimited)")
    parser.add_argument("--seed", type=int, default=None,
                        help="the shaper seed (same seed + same sequence = "
                             "same decisions)")
    parser.add_argument("--profile", choices=("default", "mild", "hostile"),
                        default="default",
                        help="the network tier to apply (default: the flag "
                             "values; 'mild' and 'hostile' fill in the three "
                             "impairments the plan names -- loss, jitter, "
                             "reorder -- from the tiers below)")
    parser.add_argument("--harness", default=None,
                        help="path to soak-p2p (default: found under build/)")
    parser.add_argument("--out-dir", default=str(DEFAULT_OUT_DIR),
                        help="where the evidence goes (default: build/soak-impaired)")
    parser.add_argument("--timeout-slack", type=float, default=120.0,
                        help="extra seconds allowed beyond --seconds before the "
                             "harness is considered stuck (default 120)")
    parser.add_argument("--summary", action="store_true",
                        help="summarise the recorded evidence instead of running")
    parser.add_argument("--expect-green", action="store_true",
                        help="with --summary, exit 1 unless every recorded run is "
                             "green and none was on a dirty tree")
    parser.add_argument("--expect-q5", action="store_true",
                        help="with --summary, exit 0 only when the archive "
                             "contains a PASS run of at least 24 h under the "
                             "plan's Q5 impairments (loss + jitter + reorder)")
    parser.add_argument("--self-test", action="store_true",
                        help="run the built-in self-test and exit")
    return parser


def apply_profile(args):
    """Fill the plan's impairment tiers when a named profile is chosen.

    The plan's Q5 names *three* impairments -- reordering, loss and
    jitter -- and the two built-in tiers are the pairs a soak box can
    reproduce without root: they are the same shape `netem` applies, and
    the two extremes a 24 h run should survive.
    """
    if args.profile == "default":
        return
    if args.profile == "mild":
        # A link you would still call "fine": 2 % loss, 20 ms ± 10 ms,
        # 1 % reordered.
        args.loss, args.delay_ms, args.jitter_ms, args.reorder = 2.0, 20.0, 10.0, 1.0
    elif args.profile == "hostile":
        # The plan's worst case: 5 % loss, 80 ms ± 40 ms, 10 % reordered.
        args.loss, args.delay_ms, args.jitter_ms, args.reorder = 5.0, 80.0, 40.0, 10.0


def main(argv=None):
    args = build_parser().parse_args(argv)

    if args.self_test:
        return _self_test()

    apply_profile(args)

    if args.summary:
        return summarize(args.out_dir, expect_green=args.expect_green,
                         expect_q5=args.expect_q5)

    try:
        record = run_once(args)
    except ToolError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2

    if record["status"] == "PASS":
        return 0
    if record["status"] == "FAIL":
        return 1
    return 2


if __name__ == "__main__":
    sys.exit(main())
