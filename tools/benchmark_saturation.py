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
  4. **The estimate stayed bounded.**  The bandwidth estimate did not
     climb while the link was congested.  This is the regression guard
     for the AIMD defect the benchmark itself uncovered: the estimator
     used to resume its additive increase after the cooldown expired
     even though the link was *still* lossy, so the estimate walked
     back up to (and past) the rate that had just been throttled.
  5. **The offered load stayed sane.**  The delivered rate did not
     exceed the link's capacity by a wide margin — the ladder
     converged, it did not oscillate into a runaway.
  6. **The ladder recovered.**  Once the link freed up, the ladder
     climbed back and the stream got faster.
  7. **The ladder did not flap.**  The number of level changes is
     bounded — a ladder that oscillates every second is not "surviving
     congestion", it is thrashing.

Every invariant is reported in the artifact with its measured value,
so a failure is diagnosable from the committed file alone.

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
#: multiple.  The link's token bucket holds up to one second of credit,
#: so a short window can legitimately deliver more than the nominal
#: capacity; the slack covers that and nothing more.
DELIVERED_OVER_CAPACITY_TOLERANCE = 1.30

#: How much the bandwidth estimate may exceed its starting value while
#: the link is congested, as a multiple.  A congestion-control estimate
#: that climbs while the link is congested is the AIMD defect this
#: benchmark was written to catch, so the bound is tight.
ESTIMATE_DRIFT_TOLERANCE = 1.10

#: The largest number of ladder moves a run may make.  A ladder that
#: oscillates on the 500 ms reporting interval is thrashing, not
#: adapting; the bound is loose enough for the legitimate two-step
#: descent and ascent the scenarios produce.
MAX_LEVEL_CHANGES = 12


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

def check_invariants(doc: dict) -> list[dict]:
    """Check every invariant the run must satisfy.

    Each entry records the invariant's name, whether it held, the
    measured value and the limit it was measured against, so a failure
    is diagnosable from the artifact alone.
    """
    summary = doc.get("summary", {})
    benchmark = doc.get("benchmark", {})
    curve = doc.get("curve", [])

    capacity = float(benchmark.get("capacity_bps", 0) or 0)
    saturation_s = float(benchmark.get("saturation_s", 0) or 0)

    # The estimate the run started from: the first sample's estimate.
    initial_estimate = float(curve[0].get("estimate_bps", 0)) if curve else 0.0

    # The highest estimate seen while the link was congested.
    peak_estimate = 0.0
    for s in curve:
        if float(s.get("t_s", 0.0)) <= saturation_s:
            peak_estimate = max(peak_estimate, float(s.get("estimate_bps", 0)))

    saturated_delivered = float(summary.get("saturated_delivered_bps", 0))
    saturated_sent = float(summary.get("saturated_sent_bps", 0))
    recovered_delivered = float(summary.get("recovered_delivered_bps", 0))
    estimate_after = float(summary.get("estimate_after_saturation_bps", 0))

    checks: list[dict] = []

    def add(name: str, ok: bool, measured, limit, detail: str) -> None:
        checks.append({
            "invariant": name,
            "ok": bool(ok),
            "measured": measured,
            "limit": limit,
            "detail": detail,
        })

    # 1. The saturation was real: packets were dropped.
    dropped = int(summary.get("packets_dropped", 0))
    add("saturation_was_real", dropped > 0, dropped, "> 0",
        "the link dropped packets, so it was genuinely at capacity")

    # 2. The stream survived: the receiver kept getting data.
    add("stream_survived", saturated_delivered > 0,
        int(saturated_delivered), "> 0",
        "the receiver kept receiving data throughout the saturation phase")

    # 3. The ladder reacted: it dropped at least one rung.
    drops = int(summary.get("level_drops", 0))
    add("ladder_reacted", drops >= 1, drops, ">= 1",
        "the ladder stepped down when the link saturated")

    # 4. The estimate stayed bounded while the link was congested.
    #    This is the regression guard for the AIMD defect.
    drift_limit = initial_estimate * ESTIMATE_DRIFT_TOLERANCE
    add("estimate_stayed_bounded", peak_estimate <= drift_limit,
        int(peak_estimate), f"<= {int(drift_limit)}",
        "the bandwidth estimate did not climb while the link was congested")
    add("estimate_did_not_rise_at_the_end", estimate_after <= drift_limit,
        int(estimate_after), f"<= {int(drift_limit)}",
        "the estimate at the end of the saturation phase was still bounded")

    # 5. The delivered rate did not exceed the link's capacity.
    delivered_limit = capacity * DELIVERED_OVER_CAPACITY_TOLERANCE
    add("delivered_stayed_within_capacity",
        saturated_delivered <= delivered_limit,
        int(saturated_delivered), f"<= {int(delivered_limit)}",
        "the ladder converged onto the link's capacity instead of "
        "overshooting it")

    # 6. The ladder recovered and the stream got faster.
    rises = int(summary.get("level_rises", 0))
    add("ladder_recovered", rises >= 1, rises, ">= 1",
        "the ladder climbed back once the link freed up")
    add("throughput_recovered", recovered_delivered > saturated_delivered,
        int(recovered_delivered), f"> {int(saturated_delivered)}",
        "the stream carried more data after the recovery than during the "
        "saturation")

    # 7. The ladder did not flap.
    changes = int(summary.get("level_changes", 0))
    add("ladder_did_not_flap", changes <= MAX_LEVEL_CHANGES,
        changes, f"<= {MAX_LEVEL_CHANGES}",
        "the ladder moved a bounded number of times")

    return checks


def all_invariants_hold(checks: list[dict]) -> bool:
    return all(c["ok"] for c in checks)


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
            mark = "ok  " if c["ok"] else "FAIL"
            print(f"    [{mark}] {c['invariant']:<32} "
                  f"measured {c['measured']}, limit {c['limit']}")
        print()

    verdict = doc.get("verdict", {})
    print(f"verdict: {verdict.get('all_invariants_hold')}")
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

    for spec in SCENARIOS:
        raw = run_benchmark(binary, args, spec["min_height"])
        checks = check_invariants(raw)
        for c in checks:
            if not c["ok"]:
                failures.append({"scenario": spec["name"], **c})
        scenarios.append({
            "name": spec["name"],
            "title": spec["title"],
            "why": spec["why"],
            "config": raw.get("benchmark", {}),
            "summary": raw.get("summary", {}),
            "invariants": checks,
            "curve": raw.get("curve", []),
        })
        print(f"ran scenario '{spec['name']}' "
              f"({sum(1 for c in checks if c['ok'])}/{len(checks)} invariants)")

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
            "The estimate's bound during the saturation phase is the "
            "regression guard for an AIMD defect this benchmark uncovered: "
            "the estimator used to resume its additive increase once the "
            "cooldown expired even while the link was still lossy, so the "
            "estimate walked back up to the rate that had just been "
            "throttled. The Goog-CC front-end already carried that guard; "
            "the AIMD one did not.",
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
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
