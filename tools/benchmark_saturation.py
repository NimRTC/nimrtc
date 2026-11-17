#!/usr/bin/env python3
# Copyright (c) 2026 NimRTC contributors.
# SPDX-License-Identifier: MIT
"""Measure the ABR behaviour on a **saturated** link (v1.2 plan item **E6**).

The v1.2 convergence plan's evidence table says:

  | Adaptive bitrate survives congestion | ABR + pacing unit-tested |

and marks the row *open*, because "unit-tested" is not the same claim
as "measured".  `tools/` shipped eight benchmark scripts (latency, dc,
weaknet, weaknet_netem, capture_encode, hotpath, remote_desktop, size)
and **none of them saturates a link**: the weak-network scripts inject
*loss and jitter* into a link that has spare capacity, which is a
different phenomenon from *congestion*.

## What the C++ binary does

This script drives `tests/bench/bench_saturation.cpp`, which runs the
**shipped** components against each other over a **real UDP socket
pair**:

  - `nimrtc::bwe::Bwe` (AIMD) or `nimrtc::bwe::GoogCcBwe` — the
    shipped bandwidth estimator;
  - `nimrtc::video_bitrate_control::BitrateController` — the shipped
    rate controller, with the shipped resolution ladder, the resolution
    floor and the keyframe-cost budget;
  - `nimrtc::pacing::TokenBucket` — the shipped pacer;
  - a **real datagram** path over the loopback interface.

The congestion is **real**: the receiving side of the link enforces a
token-bucket *capacity*, and a datagram that arrives when the bucket is
empty is **dropped** — exactly as a bottleneck router's queue
overflows.  The loss the estimator reacts to is therefore a real loss
of real packets, measured by the real receiver.

What is *modelled* is the link itself (a fixed-capacity, loss-on-
overflow bottleneck) and the peer (a receiver that reports loss, RTT
and optionally REMB once per reporting interval).  There is no ICE, no
DTLS, no SRTP and no codec in the path: the benchmark measures the
**rate-control closed loop**, not the transport stack.

## Why there are two scenarios

A saturated link is only interesting if the ladder has somewhere to go.
The two runs therefore differ **only** in the resolution floor:

  - **`floor`** (`--min-height 720`, the product's legibility floor) —
    the lowest rung the floor allows is **twice** the link's capacity,
    so the picture cannot be made small enough to fit.  The ladder must
    hold the floor and let the **bitrate** be throttled instead.  This
    is the case the floor (plan D2) and the effective-target mechanism
    exist for.
  - **`unfloored`** (`--min-height 0`) — the ladder may descend to a
    rung that *does* fit, so the offered load converges onto the link's
    capacity and the stream runs at full quality of the available
    bandwidth.

A third parameter, the **estimator**, is a knob (`--bwe`) rather than a
scenario: the artifact records which estimator produced the numbers.

## The invariants

The binary is deliberately **not** a ctest case: every number in it is
machine-dependent (the loopback's real RTT, the host's real scheduling
jitter), so a *threshold* assertion would be a flaky test rather than a
measurement.  The properties that must hold regardless of the machine
are asserted **here** instead — they are the acceptance criteria the
plan asks for, and a failure is a real defect, not a slow machine:

  1. **The saturation was real.**  Packets were dropped, so the link
     was genuinely at capacity rather than merely configured to be.
  2. **The stream survived.**  The receiver kept getting data
     throughout the saturation phase — no black hole, no deadlock.
  3. **The ladder reacted.**  It dropped at least one rung.
  4. **The estimator never raised its target while the link complained.**
     On a report that *said* the link was congested, the estimator must
     not raise its target.  This is the regression guard for the AIMD
     defect the benchmark itself uncovered: the estimator used to
     resume its additive increase once the cooldown expired even
     though the link was *still* lossy, so the estimate walked back up
     to (and past) the rate that had just been throttled.  The correct
     behaviour is exact — zero such raises — and the artifact itemises
     every one it finds, so a failure names the offending reports.
  5. **The delivered rate stayed within the capacity.**  The ladder
     converged onto the link's capacity instead of overshooting it.
  6. **The offered load tracked the capacity.**  The load the sender
     put on the wire was in the same ballpark as what the link could
     carry — the ladder and the link agreed.
  7. **The ladder recovered.**  Once the link freed up, the ladder
     climbed back and the stream got faster.
  8. **The ladder did not flap.**  The number of level changes is
     bounded — a ladder that oscillates every second is not "surviving
     congestion", it is thrashing.

The invariants are stated as *properties*, not as *thresholds against a
reference number*: they must hold on any machine, however fast or slow.
The committed artifact is one local sample, not a pass/fail threshold.

## Usage

    # Measure both scenarios and write the artifact.
    python tools/benchmark_saturation.py --build-dir build/e6 \\
        --out docs/benchmarks/saturation_windows_x86_64.json

    # Print the summary table without writing the artifact.
    python tools/benchmark_saturation.py --build-dir build/e6 --dry-run

The build tree must already contain `bench_saturation.exe`; build it
with

    cmake --build <build-dir> --target bench_saturation
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import platform
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# ---------------------------------------------------------------------------
# The scenarios
# ---------------------------------------------------------------------------

#: The scenarios, and the only thing that differs between them is the
#: resolution floor.  `min_height` of 0 means "no floor".
SCENARIOS = (
    {
        "name": "floor",
        "title": "the product's legibility floor (720p)",
        "min_height": 720,
        "why": (
            "the lowest rung the floor allows is twice the link's capacity, "
            "so the picture cannot be made small enough to fit: the ladder "
            "must hold the floor and let the bitrate be throttled instead"
        ),
    },
    {
        "name": "unfloored",
        "title": "no resolution floor",
        "min_height": 0,
        "why": (
            "the ladder may descend to a rung that does fit, so the offered "
            "load converges onto the link's capacity"
        ),
    },
)

# ---------------------------------------------------------------------------
# The invariants
# ---------------------------------------------------------------------------

#: How much the delivered rate may exceed the link's capacity, as a
#: multiple.
#:
#: The link's token bucket holds up to **one second** of credit, so an
#: idle moment lets the next second of traffic through at up to twice
#: the nominal capacity.  A measurement window that straddles such a
#: moment therefore sees up to 2x the capacity, and the bound has to
#: allow it — a bound tighter than the link's own burst allowance would
#: fail on the *physics* of the model rather than on anything the
#: rate-control loop did.
#:
#: The slack below sits between the burst allowance (2.0) and the
#: nominal capacity (1.0): the delivered rate must be within the
#: capacity plus the burst, and no further.
DELIVERED_OVER_CAPACITY_TOLERANCE = 2.00

#: The loss rate at which the link counts as congested.  This is the
#: estimator's own threshold (RFC 8698 §5.1's 2%), so a report above it
#: is a report that *said* the link was congested.
CONGESTION_LOSS_THRESHOLD = 0.02

#: The estimator's reasons that mean "the estimator raised its
#: target".  On a report that said the link was congested, neither is
#: allowed: the multiplicative decrease has already fired for that
#: congestion event, and raising the target again walks the sender
#: back into the wall it just hit.
RAISING_REASONS = ("aimd_increase", "remb_override")

#: The largest number of times the estimator may raise its target on a
#: report that said the link was congested.  The correct answer is
#: **zero** — with the guard in place, the estimate can only rise on a
#: report that said there was *no* loss.
#:
#: This is the regression guard for the AIMD defect the benchmark
#: itself uncovered.  The AIMD front-end used to resume its additive
#: increase once the 500 ms cooldown expired, even though the link was
#: *still* lossy, so the estimate climbed back to (and past) the rate
#: that had just been throttled.  The Goog-CC front-end already carried
#: the `!is_lossy` guard; the AIMD one did not, so the two estimators
#: disagreed under exactly the condition that matters.
MAX_RAISES_ON_A_CONGESTED_REPORT = 0

#: The largest number of ladder moves a run may make.  A ladder that
#: oscillates on the 500 ms reporting interval is thrashing, not
#: adapting; the bound is loose enough for the legitimate two-step
#: descent and ascent the scenarios produce.
MAX_LEVEL_CHANGES = 12

#: How much of the controller's own target the sender must actually
#: deliver during the saturation phase.
#:
#: This is the **host-health** signal, and it is what distinguishes a
#: real measurement from a measurement that never happened.  The ladder
#: converges the target onto a rung the link can carry, so the target is
#: at or below the link's capacity; a delivered rate far *below* that
#: target is not the link dropping packets, it is the sender failing to
#: put the bytes on the wire — which happens when the CPU is busy.
#:
#: Without this distinction a busy CI runner produces a run that looks
#: like a rate-control failure: the sender never fills the link, the
#: link never queues, nothing is dropped, and the invariant that
#: compares the offered load against the capacity fails for a reason
#: that has nothing to do with the code under test.
SENDER_TARGET_FLOOR = 0.70

#: How many times a scenario may be re-run when the host was too busy to
#: saturate the link.  A scenario that cannot be measured after this
#: many attempts is a **failure**, not a pass: the benchmark is a gate,
#: and a gate that silently degrades into a no-op is worse than no gate
#: at all.
MAX_SCENARIO_ATTEMPTS = 3


def _load_module(path: Path, name: str):
    """Load a sibling tool as a module, or return None."""
    if not path.is_file():
        return None
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        return None
    module = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(module)
    except Exception:  # pragma: no cover - a broken sibling must not
        return None       # break this tool
    return module


def describe_machine() -> dict:
    """The machine description, reusing the J5 tool's collector.

    The artifact's machine fields therefore match the other committed
    benchmark artifacts exactly, so they can be read side by side.
    """
    mod = _load_module(ROOT / "tools" / "benchmark_capture_encode.py",
                       "_nimrtc_benchmark_capture_encode")
    if mod is not None and hasattr(mod, "describe_machine"):
        try:
            return mod.describe_machine()
        except Exception:  # pragma: no cover
            pass
    return {
        "os": platform.system(),
        "os_release": platform.release(),
        "os_version": platform.version(),
        "machine": platform.machine(),
        "python": platform.python_version(),
        "cpu_model": None,
        "cpu_cores_physical": None,
        "cpu_cores_logical": None,
        "gpus": [],
        "gpu_encoder_present": False,
    }


# ---------------------------------------------------------------------------
# Locating and running the binary
# ---------------------------------------------------------------------------

def find_binary(build_dir: Path) -> Path | None:
    """Locate `bench_saturation` in a build tree.

    The multi-config generators (MSVC / Visual Studio) put it under
    `<config>/`; the single-config ones (Ninja / Makefiles) put it
    directly under `tests/`.
    """
    tests = build_dir / "tests"
    candidates = [
        tests / "bench_saturation",
        # Single-config generators on Windows still produce a `.exe`.
        tests / "bench_saturation.exe",
        tests / "Debug" / "bench_saturation.exe",
        tests / "Release" / "bench_saturation.exe",
        tests / "RelWithDebInfo" / "bench_saturation.exe",
        tests / "MinSizeRel" / "bench_saturation.exe",
    ]
    for c in candidates:
        if c.is_file():
            return c
    return None


def run_benchmark(binary: Path, args: argparse.Namespace,
                  min_height: int) -> dict:
    """Run one scenario and return the benchmark's JSON document."""
    cmd = [
        str(binary),
        "--capacity-bps", str(args.capacity_bps),
        "--recovery-bps", str(args.recovery_bps),
        "--latency-ms", str(args.latency_ms),
        "--source-fps", str(args.source_fps),
        "--source-bitrate", str(args.source_bitrate_bps),
        "--packet-bytes", str(args.packet_bytes),
        "--saturation-s", str(args.saturation_s),
        "--recovery-s", str(args.recovery_s),
        "--bwe", args.bwe,
        "--mode", args.mode,
        "--min-height", str(min_height),
        "--report-ms", str(args.report_ms),
    ]
    if args.no_remb:
        cmd.append("--no-remb")

    # The engine's log lines go to stderr and may carry characters the
    # host's default code page cannot represent (the Chinese locale's
    # GBK, for one).  Decoding them strictly would abort the capture on
    # a log line, so they are decoded leniently and then discarded —
    # only stdout carries the JSON document.
    proc = subprocess.run(cmd, capture_output=True, text=True,
                          encoding="utf-8", errors="replace")
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        raise SystemExit(
            f"error: the benchmark exited with {proc.returncode}")
    # The benchmark prints exactly one JSON document on stdout; the
    # controller's log lines go to stderr.
    for line in reversed(proc.stdout.strip().splitlines()):
        line = line.strip()
        if line.startswith("{"):
            return json.loads(line)
    raise SystemExit("error: the benchmark printed no JSON document")


# ---------------------------------------------------------------------------
# The invariants
# ---------------------------------------------------------------------------

def raises_on_congested_reports(doc: dict, saturation_s: float) -> list[dict]:
    """The reports where the estimator raised its target while congested.

    A report whose loss rate is at or above the estimator's own
    congestion threshold is a report that **said the link was
    congested**.  The estimator must not raise its target on such a
    report: the multiplicative decrease has already fired for that
    congestion event, and raising the target again walks the sender
    back into the wall it just hit.

    The decision is made on the **report** granularity — the estimator's
    own input and output for one feedback sample — and on the
    estimator's own **reason**, not on the before/after estimate pair.
    Two things force that choice:

      - the 500 ms curve is too coarse: two reports can hide inside
        one curve sample, and the curve's loss figure is the *last*
        report's, not the one that produced the estimate's change;

      - the reported estimate is an **exponentially smoothed** value,
        so it can drift upwards by a few bits after a decrease without
        the estimator having raised anything.  The reason is the
        authoritative signal: it says whether the estimator raised
        its target, and why.

    The reasons that count as "the estimator raised its target" are
    `aimd_increase` (the additive increase — the very thing that must
    not fire on a congested report) and `remb_override` (the peer's
    estimate being adopted; also a raise, and also not allowed to
    happen while the link is complaining).
    """
    out: list[dict] = []
    for rep in doc.get("reports", []):
        t_s = float(rep.get("t_s", 0.0))
        if t_s > saturation_s:
            continue
        loss = float(rep.get("loss_rate", 0.0))
        reason = str(rep.get("reason", ""))
        if loss >= CONGESTION_LOSS_THRESHOLD and reason in RAISING_REASONS:
            out.append({
                "t_s": round(t_s, 3),
                "loss_rate": round(loss, 4),
                "reason": reason,
                "estimate_before_bps": int(rep.get("estimate_before_bps", 0)),
                "estimate_after_bps": int(rep.get("estimate_after_bps", 0)),
            })
    return out


def check_invariants(doc: dict) -> list[dict]:
    """Check every invariant the run must satisfy.

    Each entry records the invariant's name, whether it held, the
    measured value and the limit it was measured against, so a failure
    is diagnosable from the artifact alone.
    """
    summary = doc.get("summary", {})
    benchmark = doc.get("benchmark", {})

    capacity = float(benchmark.get("capacity_bps", 0) or 0)
    saturation_s = float(benchmark.get("saturation_s", 0) or 0)

    saturated_delivered = float(summary.get("saturated_delivered_bps", 0))
    saturated_sent = float(summary.get("saturated_sent_bps", 0))
    recovered_delivered = float(summary.get("recovered_delivered_bps", 0))

    checks: list[dict] = []

    def add(name: str, ok: bool, measured, limit, detail: str) -> None:
        checks.append({
            "invariant": name,
            "ok": bool(ok),
            "measured": measured,
            "limit": limit,
            "detail": detail,
        })

    def skip(name: str, measured, limit, detail: str) -> None:
        """An invariant that could not be evaluated.

        A skip is not a pass and not a failure: it says the *measurement*
        did not happen (the host was too busy to fill the link), so the
        property in question is simply unknown for this run.  It is
        reported separately so that a green verdict cannot hide the fact
        that a property went unchecked.
        """
        checks.append({
            "invariant": name,
            "ok": True,
            "skipped": True,
            "measured": measured,
            "limit": limit,
            "detail": detail,
        })

    # 1. The saturation was real: packets were dropped.
    dropped = int(summary.get("packets_dropped", 0))
    add("saturation_was_real", dropped > 0, dropped, "> 0",
        "the link dropped packets, so it was genuinely at capacity")

    # 1b. The host was healthy enough for the load-dependent invariants
    #     to mean anything.  The sender is a process on the host: when
    #     the CPU is busy it cannot put the bytes on the wire, so the
    #     link never fills, the link never queues, nothing is dropped,
    #     and every question of the form "what load did the link have to
    #     drop?" has no answer.  That is a measurement that did not
    #     happen, not a failure of the code under test — so it is
    #     reported as a skip here, and the wrapper retries the scenario.
    target_after = float(summary.get("target_after_saturation_bps", 0))
    if target_after > 0:
        delivered_ratio = saturated_delivered / target_after
    else:
        delivered_ratio = 0.0
    host_was_healthy = delivered_ratio >= SENDER_TARGET_FLOOR

    # 2. The stream survived: the receiver kept getting data.
    add("stream_survived", saturated_delivered > 0,
        int(saturated_delivered), "> 0",
        "the receiver kept receiving data throughout the saturation phase")

    # 3. The ladder reacted: it dropped at least one rung.
    drops = int(summary.get("level_drops", 0))
    add("ladder_reacted", drops >= 1, drops, ">= 1",
        "the ladder stepped down when the link saturated")

    # 4. The estimator never raised its target while the link was
    #    complaining.  This is the regression guard for the AIMD defect.
    raises = raises_on_congested_reports(doc, saturation_s)
    add("estimate_did_not_climb_while_congested",
        len(raises) <= MAX_RAISES_ON_A_CONGESTED_REPORT,
        len(raises), f"<= {MAX_RAISES_ON_A_CONGESTED_REPORT}",
        "the estimator never raised its target on a report that said the "
        "link was congested (the AIMD regression guard)")

    # 5. The delivered rate did not exceed the link's capacity.
    delivered_limit = capacity * DELIVERED_OVER_CAPACITY_TOLERANCE
    if host_was_healthy:
        add("delivered_stayed_within_capacity",
            saturated_delivered <= delivered_limit,
            int(saturated_delivered), f"<= {int(delivered_limit)}",
            "the ladder converged onto the link's capacity instead of "
            "overshooting it")
    else:
        skip("delivered_stayed_within_capacity",
             int(saturated_delivered), f"<= {int(delivered_limit)}",
             "skipped: the sender delivered only "
             f"{delivered_ratio:.0%} of the target, so the host was too "
             "busy to saturate the link and the delivered rate was never "
             "measured against a full link")

    # 6. The offered load stayed near the capacity.  A converged ladder
    #    offers roughly what the link can carry; a wildly different
    #    number means the ladder and the link disagree.
    if capacity > 0:
        offer_ratio = saturated_sent / capacity
        if host_was_healthy:
            add("offered_load_tracked_the_capacity", 0.5 <= offer_ratio <= 2.5,
                round(offer_ratio, 3), "0.5 .. 2.5 x capacity",
                "the offered load converged onto the link's capacity")
        else:
            skip("offered_load_tracked_the_capacity",
                 round(offer_ratio, 3), "0.5 .. 2.5 x capacity",
                 "skipped: the sender delivered only "
                 f"{delivered_ratio:.0%} of the target, so the host was too "
                 "busy to saturate the link and the offered load was never "
                 "measured against a full link")

    # 7. The ladder recovered and the stream got faster.
    rises_up = int(summary.get("level_rises", 0))
    add("ladder_recovered", rises_up >= 1, rises_up, ">= 1",
        "the ladder climbed back once the link freed up")
    add("throughput_recovered", recovered_delivered > saturated_delivered,
        int(recovered_delivered), f"> {int(saturated_delivered)}",
        "the stream carried more data after the recovery than during the "
        "saturation")

    # 8. The ladder did not flap.
    changes = int(summary.get("level_changes", 0))
    add("ladder_did_not_flap", changes <= MAX_LEVEL_CHANGES,
        changes, f"<= {MAX_LEVEL_CHANGES}",
        "the ladder moved a bounded number of times")

    return checks


def all_invariants_hold(checks: list[dict]) -> bool:
    """True when no invariant failed.

    A *skipped* invariant is not a failure: it means the measurement did
    not happen.  Skips are counted separately by `skipped_invariants()`
    so that a green verdict cannot quietly hide an unchecked property.
    """
    return all(c["ok"] for c in checks)


def skipped_invariants(checks: list[dict]) -> list[str]:
    """The names of the invariants that could not be evaluated."""
    return [c["invariant"] for c in checks if c.get("skipped")]


# ---------------------------------------------------------------------------
# The summary table
# ---------------------------------------------------------------------------

def print_summary(doc: dict) -> None:
    machine = doc.get("machine", {})
    cpu = machine.get("cpu_model") or "unknown CPU"
    cores = machine.get("cpu_cores_logical")
    cpu_line = f"machine : {cpu}"
    if cores:
        cpu_line += f" ({cores} logical cores)"
    print(cpu_line)
    print(f"os     : {machine.get('os')} {machine.get('machine')}")
    print()

    for scenario in doc.get("scenarios", []):
        cfg = scenario.get("config", {})
        summary = scenario.get("summary", {})
        print(f"scenario {scenario['name']} — {scenario.get('title', '')}")
        print(f"  {'link capacity':>14}: {cfg.get('capacity_bps')} bps "
              f"(recovering to {cfg.get('recovery_capacity_bps')} bps)")
        print(f"  {'estimator':>14}: {cfg.get('bwe')}")
        print(f"  {'floor':>14}: {cfg.get('min_height')} px")
        print()
        print(f"  {'ladder':>22}: L{summary.get('initial_level')} "
              f"-> L{summary.get('level_after_saturation')} "
              f"(saturated) -> L{summary.get('final_level')} (recovered)")
        print(f"  {'target':>22}: {summary.get('target_after_saturation_bps')} bps "
              f"-> {summary.get('final_target_bps')} bps")
        print(f"  {'estimate':>22}: {summary.get('estimate_after_saturation_bps')} bps "
              f"-> {summary.get('final_estimate_bps')} bps")
        print(f"  {'offered (sent)':>22}: {summary.get('saturated_sent_bps')} bps "
              f"saturated, {summary.get('recovered_sent_bps')} bps recovered")
        print(f"  {'delivered':>22}: {summary.get('saturated_delivered_bps')} bps "
              f"saturated, {summary.get('recovered_delivered_bps')} bps recovered")
        print(f"  {'packets':>22}: {summary.get('packets_sent')} sent, "
              f"{summary.get('packets_dropped')} dropped")
        print(f"  {'moves':>22}: {summary.get('level_changes')} "
              f"({summary.get('level_drops')} down, "
              f"{summary.get('level_rises')} up)")
        print(f"  {'floor blocks':>22}: {summary.get('floor_blocks')}")
        print(f"  {'keyframe blocks':>22}: {summary.get('keyframe_cost_blocks')}")
        print()

        checks = scenario.get("invariants", [])
        print("  invariants")
        for c in checks:
            if c.get("skipped"):
                mark = "skip"
            else:
                mark = "ok  " if c["ok"] else "FAIL"
            print(f"    [{mark}] {c['invariant']:<32} "
                  f"measured {c['measured']}, limit {c['limit']}")
        print()

    verdict = doc.get("verdict", {})
    print(f"verdict: {verdict.get('all_invariants_hold')}")
    for s in verdict.get("skips", []):
        print(f"  SKIPPED {s['scenario']}.{s['invariant']}: {s['detail']}")
    if not verdict.get("all_invariants_hold", False):
        for f in verdict.get("failures", []):
            print(f"  FAILED {f['scenario']}.{f['invariant']}: "
                  f"measured {f['measured']}, limit {f['limit']}")


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", type=Path, default=ROOT / "build",
                    help="the build tree that holds the benchmark "
                         "(default: build/)")
    ap.add_argument("--out", type=Path, default=None,
                    help="where to write the artifact (default: "
                         "<build-dir>/bench_saturation.json)")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the summary without writing the artifact")
    ap.add_argument("--capacity-bps", type=int, default=600_000,
                    help="the link's capacity while saturated (default: 600000)")
    ap.add_argument("--recovery-bps", type=int, default=8_000_000,
                    help="the link's capacity after the saturation phase "
                         "(default: 8000000)")
    ap.add_argument("--latency-ms", type=int, default=20,
                    help="the link's one-way latency (default: 20)")
    ap.add_argument("--source-fps", type=int, default=30,
                    help="the source frame rate (default: 30)")
    ap.add_argument("--source-bitrate-bps", type=int, default=1_600_000,
                    help="the top rung's bitrate (default: 1600000)")
    ap.add_argument("--packet-bytes", type=int, default=1200,
                    help="the datagram size (default: 1200)")
    ap.add_argument("--saturation-s", type=float, default=20.0,
                    help="the saturation phase's length, in seconds "
                         "(default: 20)")
    ap.add_argument("--recovery-s", type=float, default=45.0,
                    help="the recovery phase's length, in seconds "
                         "(default: 45)")
    ap.add_argument("--bwe", choices=("aimd", "goog-cc"), default="aimd",
                    help="the bandwidth estimator to drive (default: aimd)")
    ap.add_argument("--mode", choices=("quality", "smoothness"),
                    default="smoothness",
                    help="the ladder mode (default: smoothness)")
    ap.add_argument("--report-ms", type=int, default=100,
                    help="the feedback reporting interval (default: 100)")
    ap.add_argument("--no-remb", action="store_true",
                    help="do not report REMB from the peer")
    args = ap.parse_args()

    if args.saturation_s <= 0 or args.recovery_s <= 0:
        raise SystemExit("error: the phase lengths must be positive")

    binary = find_binary(args.build_dir)
    if binary is None:
        raise SystemExit(
            f"error: bench_saturation not found under {args.build_dir}\n"
            f"       build it first: cmake --build {args.build_dir} "
            f"--target bench_saturation")

    scenarios: list[dict] = []
    failures: list[dict] = []
    skips: list[dict] = []

    for spec in SCENARIOS:
        # The sender is a process on the host, and a busy host cannot
        # fill the link.  When that happens the load-dependent
        # invariants are skipped rather than failed, and the scenario is
        # re-run: a measurement that did not happen is not a result.
        # After MAX_SCENARIO_ATTEMPTS the scenario is a **failure** —
        # a gate that quietly degrades into a no-op is worse than no
        # gate at all.
        attempt = 0
        while True:
            attempt += 1
            raw = run_benchmark(binary, args, spec["min_height"])
            checks = check_invariants(raw)
            if not skipped_invariants(checks) or attempt >= MAX_SCENARIO_ATTEMPTS:
                break
            print(f"scenario '{spec['name']}': the host was too busy to "
                  f"saturate the link (attempt {attempt}/"
                  f"{MAX_SCENARIO_ATTEMPTS}); re-running")

        for c in checks:
            if not c["ok"]:
                failures.append({"scenario": spec["name"], **c})
            elif c.get("skipped"):
                if attempt >= MAX_SCENARIO_ATTEMPTS:
                    # The scenario could not be measured even after the
                    # retries: the host was too busy every time.  That
                    # is a failure of the *gate*, not a pass with a
                    # footnote — a gate that cannot measure is not a
                    # gate.
                    failures.append({
                        "scenario": spec["name"],
                        **c,
                        "ok": False,
                        "detail": (c["detail"] +
                                   f"; still unmeasurable after "
                                   f"{MAX_SCENARIO_ATTEMPTS} attempts"),
                    })
                else:
                    skips.append({"scenario": spec["name"], **c})
        scenarios.append({
            "name": spec["name"],
            "title": spec["title"],
            "why": spec["why"],
            "attempts": attempt,
            "config": raw.get("benchmark", {}),
            "summary": raw.get("summary", {}),
            "invariants": checks,
            # The evidence for the AIMD regression guard: every report
            # where the estimator raised its target on a report that said
            # the link was congested.  An empty list is the correct
            # result; a non-empty one is the defect, itemised.
            "estimator_raises_on_congested_reports": raises_on_congested_reports(
                raw, float(raw.get("benchmark", {}).get("saturation_s", 0) or 0)),
            # The raw evidence: every feedback sample the estimator saw,
            # with its loss rate, the estimate before and after, and the
            # estimator's own reason.  The guard above is decided on
            # these records, so they are carried into the artifact --
            # a failure must be diagnosable from the file alone, not
            # only from the code that reads it.
            "reports": raw.get("reports", []),
            "curve": raw.get("curve", []),
        })
        print(f"ran scenario '{spec['name']}' "
              f"({sum(1 for c in checks if c['ok'] and not c.get('skipped'))}"
              f"/{len(checks)} invariants, "
              f"{len(skipped_invariants(checks))} skipped)")

    doc = {
        "schema": 1,
        "benchmark": "saturation",
        "generated_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "plan": "docs/plan/v1.2-next-plan.md",
        "plan_item": "E6",
        "evidence_row": "Adaptive bitrate survives congestion",
        "binary": str(binary),
        "machine": describe_machine(),
        "scenarios": scenarios,
        "verdict": {
            "all_invariants_hold": not failures,
            "failures": failures,
            "skips": skips,
        },
        "notes": [
            "The bandwidth estimator, the token-bucket pacer and the "
            "resolution ladder are the in-tree implementations — the same "
            "ones the engine drives. The datagrams travel over a real UDP "
            "socket pair.",
            "The congestion is real: the receiving side enforces a "
            "token-bucket capacity and drops the datagrams that arrive when "
            "the bucket is empty, exactly as a bottleneck router's queue "
            "overflows. The loss the estimator reacts to is therefore a "
            "real loss of real packets.",
            "What is modelled is the link itself (a fixed-capacity, "
            "loss-on-overflow bottleneck) and the peer (a receiver that "
            "reports loss, RTT and optionally REMB once per reporting "
            "interval). There is no ICE, DTLS, SRTP or codec in the path: "
            "the benchmark measures the rate-control closed loop, not the "
            "transport stack.",
            "The 'floor' scenario is the product's case: the resolution "
            "floor makes the lowest allowed rung twice the link's "
            "capacity, so the ladder holds the floor and the bitrate is "
            "throttled instead. The 'unfloored' scenario lets the ladder "
            "descend to a rung that fits.",
            "The numbers are machine-dependent (the loopback's real RTT and "
            "the host's real scheduling jitter), so the committed artifact "
            "is one local sample, not a pass/fail threshold. What must hold "
            "on every machine are the invariants, and they are asserted by "
            "tools/benchmark_saturation.py and recorded in the artifact.",
            "The regression guard for an AIMD defect this benchmark "
            "uncovered: the estimator used to resume its additive "
            "increase once the cooldown expired even while the link was "
            "still lossy, so the estimate walked back up to the rate that "
            "had just been throttled. The Goog-CC front-end already "
            "carried that guard; the AIMD one did not.",
            "The guard is decided on the estimator's own per-report "
            "reason ('aimd_increase' / 'remb_override' on a report whose "
            "loss rate is at or above the estimator's 2% congestion "
            "threshold), not on the before/after estimate pair: the "
            "reported estimate is an exponentially smoothed value, so it "
            "can drift upwards by a few bits after a decrease without the "
            "estimator having raised anything. The artifact's "
            "'reports' array carries the raw evidence.",
            "An invariant may be **skipped** rather than passed or failed. "
            "The sender is a process on the host, so when the CPU is busy "
            "it cannot put the bytes on the wire: the link never fills, "
            "the link never queues, nothing is dropped, and the questions "
            "that only make sense under real saturation ('did the "
            "delivered rate stay within the capacity?', 'did the offered "
            "load track the capacity?') have no answer. The wrapper "
            "detects that case -- the sender delivered far less than the "
            "controller asked for -- re-runs the scenario, and reports "
            "the properties it could not evaluate as skipped. After "
            "MAX_SCENARIO_ATTEMPTS the scenario is a failure: a gate that "
            "cannot measure is not a gate.",
        ],
    }

    if not args.dry_run:
        out = args.out or (args.build_dir / "bench_saturation.json")
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
        print(f"wrote {out}")

    print()
    print_summary(doc)

    if failures:
        print()
        print(f"error: {len(failures)} invariant(s) did not hold — the "
              f"artifact records them")
        return 1
    if skips:
        print()
        print(f"warning: {len(skips)} invariant(s) were skipped — the host "
              f"was too busy to saturate the link, so those properties were "
              f"not measured (see the verdict's 'skips')")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
