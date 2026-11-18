#!/usr/bin/env python3
# Copyright (c) 2026 NimRTC contributors.
# SPDX-License-Identifier: MIT
"""Measure the G2 control plane on a **saturated** link (v1.2 plan item **E6**).

The remote-desktop plan's **G2** acceptance criterion is:

> Under a saturated video path the control path's p99 stays within its
> declared budget; the two stacks carry independent traffic and the
> control stack's packets are observed on a different 5-tuple.

The unit test
(`tests/test_engine_control_channel.cpp::TwoEnginesExchangeControlMessagesOverTheRawUdpControlPath`)
proves the *wiring*: two engines really do exchange control messages
over the raw-UDP control path, and the address really does travel in the
SDP exchange.  It says nothing about the two halves of the criterion
above — a control path that works perfectly on an idle link can still be
starved by a saturated one.

This script drives `tests/bench/bench_control_saturation.cpp`, which is
the other half.  It runs **two real engines** over **real UDP sockets**:

  - the **media** plane is a `webrtc-classic` stack whose datagrams go
    through a **token-bucket bottleneck** — a datagram that arrives
    when the bucket is empty is **dropped**, exactly as a bottleneck
    router's queue overflows.  The saturation is therefore real loss of
    real packets, measured by the real receiver;
  - the **control** plane is the G2 shape: a separate `raw-udp-arq`
    stack with **its own socket**, its own ARQ session and its own
    5-tuple.  Its address travels in the session description
    (`a=x-nimrtc-control:`), so the two planes' endpoints are really
    distinct — not merely asserted to be.

Everything else is the shipped implementation: the engine, the SDP
machinery, the control channel's framing, the raw-UDP ARQ session and
the H.264 software encoder that produces the traffic that saturates the
media link.

## Why the two planes run on separate threads

This is not a convenience — it is the point of G2.  G2's claim is that
the control plane has its *own* congestion-control domain: its own
socket, its own ARQ, its own 5-tuple.  If the benchmark drove both
planes from one loop, the software H.264 encode (which costs tens of
milliseconds per 720p frame) would sit on the control plane's critical
path, and the measurement would report the *encoder's* latency rather
than the transport's.  The control producer therefore has a thread of
its own, and so does the media producer and the tick thread.

## The invariants

The binary is deliberately **not** a ctest case: every number in it is
machine-dependent (the host's real scheduling jitter, the loopback's
real RTT), so a *threshold* assertion would be a flaky test rather than
a measurement.  The properties that must hold regardless of the machine
are asserted **here** instead — they are the acceptance criteria the
plan asks for, and a failure is a real defect, not a slow machine:

  1. **The saturation was real.**  Datagrams were dropped, so the media
     link was genuinely at capacity rather than merely configured to be.
  2. **The control plane reached the profile's rate.**  A run that fell
     short would have measured a *lighter* load than the profile
     describes, and the p99 would be a number about an idle control
     path.  The rate is measured, not assumed.
  3. **The control plane's p99 stayed inside its budget.**  This is
     G2's headline claim, measured against the profile's own
     `transport.control.max_p99_ms`.
  4. **The control path was not starved.**  Every message the producer
     sent arrived — a control path that drops messages under load has
     failed even if its p99 looks good, because a dropped operator click
     is not a slow click.
  5. **The two stacks rode different 5-tuples.**  This is G2's second
     half: the control stack's packets must be observed on a different
     5-tuple than the media stack's.
  6. **The negative control still holds.**  The same binary, with the
     video path disabled, must still deliver every control message —
     so a passing saturated run cannot be an artefact of the video
     path being switched off by accident.

## Usage

    # Measure and write the artifact.
    python tools/benchmark_control_saturation.py --build-dir build/e6 \\
        --out docs/benchmarks/control_saturation_windows_x86_64.json

    # Print the summary without writing the artifact.
    python tools/benchmark_control_saturation.py --build-dir build/e6 --dry-run

The build tree must already contain `bench_control_saturation.exe`;
build it with

    cmake --build <build-dir> --target bench_control_saturation
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
# The profile
# ---------------------------------------------------------------------------

#: The remote-desktop profile's control plane.  These are the profile's
#: own numbers — the benchmark asserts against *them*, so the artifact
#: is a measurement of the shipped product's declared budget rather
#: than of a number invented by the test.
PROFILE_CONTROL_MAX_P99_MS = 50.0
PROFILE_CONTROL_FREQ_HZ = 200


def load_profile() -> dict:
    """The remote-desktop profile, or an empty dict when it is absent."""
    path = ROOT / "profiles" / "remote-desktop.json"
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return {}


def profile_control_budget(profile: dict) -> tuple[float, float]:
    """The profile's control-plane budget and rate.

    The profile is the authority; the constants above are the fallback
    for a tree without the profile file, so the tool still runs.
    """
    control = (profile.get("transport") or {}).get("control") or {}
    budget = control.get("max_p99_ms")
    freq = control.get("freq_hz")
    return (
        float(budget) if isinstance(budget, (int, float)) else PROFILE_CONTROL_MAX_P99_MS,
        float(freq) if isinstance(freq, (int, float)) else PROFILE_CONTROL_FREQ_HZ,
    )


# ---------------------------------------------------------------------------
# The invariants
# ---------------------------------------------------------------------------

#: The control plane must reach at least this fraction of the
#: profile's rate.  A run that fell further short would have measured a
#: *lighter* load than the profile describes, so its p99 would be a
#: number about an idle control path rather than about a saturated one.
#:
#: The bound is not 1.0 because the rate is produced by a *thread* on a
#: shared host: a scheduler that preempts the producer costs a
#: fraction of a percent, and a host under load costs more.  What must
#: not happen is a rate that is a *fraction* of the profile's, which is
#: what a starved producer looks like.
MIN_RATE_FRACTION = 0.90

#: The media link's capacity, in bits per second, while saturated.
DEFAULT_CAPACITY_BPS = 500_000

#: How many times a run may be re-made when the host was too busy to
#: saturate the link.  A run that cannot be measured after this many
#: attempts is a **failure**, not a pass: a gate that silently degrades
#: into a no-op is worse than no gate at all.
MAX_ATTEMPTS = 3


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
    """Locate `bench_control_saturation` in a build tree.

    The multi-config generators (MSVC / Visual Studio) put it under
    `<config>/`; the single-config ones (Ninja / Makefiles) put it
    directly under `tests/`.
    """
    tests = build_dir / "tests"
    candidates = [
        tests / "bench_control_saturation",
        # Single-config generators on Windows still produce a `.exe`.
        tests / "bench_control_saturation.exe",
        tests / "Debug" / "bench_control_saturation.exe",
        tests / "Release" / "bench_control_saturation.exe",
        tests / "RelWithDebInfo" / "bench_control_saturation.exe",
        tests / "MinSizeRel" / "bench_control_saturation.exe",
    ]
    for c in candidates:
        if c.is_file():
            return c
    return None


def run_benchmark(binary: Path, args: argparse.Namespace,
                  with_video: bool) -> dict:
    """Run one scenario and return the benchmark's JSON document."""
    cmd = [
        str(binary),
        "--capacity-bps", str(args.capacity_bps),
        "--freq-hz", str(args.freq_hz),
        "--max-p99-ms", str(args.max_p99_ms),
        "--duration-s", str(args.duration_s),
        "--tick-ms", str(args.tick_ms),
        "--width", str(args.width),
        "--height", str(args.height),
        "--fps", str(args.fps),
        "--payload-bytes", str(args.payload_bytes),
    ]
    if not with_video:
        cmd.append("--no-video")

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
    # engine's log lines go to stderr.
    for line in reversed(proc.stdout.strip().splitlines()):
        line = line.strip()
        if line.startswith("{"):
            return json.loads(line)
    raise SystemExit("error: the benchmark printed no JSON document")


# ---------------------------------------------------------------------------
# The invariants
# ---------------------------------------------------------------------------

def check_invariants(doc: dict, with_video: bool,
                     max_p99_ms: float, freq_hz: float) -> list[dict]:
    """Check every invariant the run must satisfy.

    Each entry records the invariant's name, whether it held, the
    measured value and the limit it was measured against, so a failure
    is diagnosable from the artifact alone.
    """
    summary = doc.get("summary", {})

    p99_ms = float(summary.get("control_p99_ms", 0.0))
    rate_hz = float(summary.get("control_rate_hz", 0.0))
    sent = int(summary.get("control_messages_sent", 0))
    received = int(summary.get("control_messages_received", 0))
    dropped = int(summary.get("control_messages_dropped", 0))
    media_dropped = int(summary.get("media_dropped_datagrams", 0))
    media_sent = int(summary.get("media_sent_datagrams", 0))
    distinct = bool(summary.get("five_tuples_distinct", False))

    checks: list[dict] = []

    def add(name: str, ok: bool, measured, limit, detail: str) -> None:
        checks.append({
            "invariant": name,
            "ok": bool(ok),
            "measured": measured,
            "limit": limit,
            "detail": detail,
        })

    # 1. The saturation was real: datagrams were dropped, so the media
    #    link was genuinely at capacity rather than merely configured
    #    to be.  A p99 measured against an idle link would be a vacuous
    #    pass.
    if with_video:
        add("saturation_was_real", media_dropped > 0 and media_sent > 0,
            media_dropped, "> 0",
            f"the media link dropped {media_dropped} of {media_sent} "
            "datagrams, so it was genuinely at capacity")
    else:
        add("saturation_was_real", media_dropped == 0 and media_sent == 0,
            media_dropped, "== 0",
            "the negative control carries no video, so there is "
            "nothing to saturate")

    # 2. The control plane reached the profile's rate.  A run that fell
    #    short would have measured a lighter load than the profile
    #    describes.
    min_rate = freq_hz * MIN_RATE_FRACTION
    add("control_reached_the_profile_rate", rate_hz >= min_rate,
        round(rate_hz, 1), f">= {min_rate:.1f}",
        f"the control plane ran at {rate_hz:.1f} Hz against the "
        f"profile's {freq_hz:.0f} Hz")

    # 3. G2's headline claim: the control plane's p99 stayed inside its
    #    declared budget while the media path was saturated.
    add("control_p99_stayed_within_budget", p99_ms < max_p99_ms,
        round(p99_ms, 3), f"< {max_p99_ms}",
        "the control plane's p99 stayed inside the profile's budget "
        "while the video path was saturated")

    # 4. The control path was not starved: every message arrived.  A
    #    control path that drops messages under load has failed even if
    #    its p99 looks good — a dropped operator click is not a slow
    #    click.
    add("control_path_was_not_starved",
        received == sent and dropped == 0,
        f"{received}/{sent}", f"{sent}/{sent}",
        f"every control message arrived ({dropped} dropped by the "
        "control channel's own queue)")

    # 5. G2's second half: the two stacks rode different 5-tuples.
    add("five_tuples_were_distinct", distinct, distinct, "True",
        "the control stack's packets travel on their own 5-tuple")

    return checks


def all_invariants_hold(checks: list[dict]) -> bool:
    """True when no invariant failed."""
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
        title = scenario.get("title", "")
        print(f"scenario {scenario['name']} — {title}")
        print(f"  {'link capacity':>16}: {cfg.get('capacity_bps')} bps")
        print(f"  {'control rate':>16}: {cfg.get('control_freq_hz')} Hz "
              f"(profile), {cfg.get('control_max_p99_ms')} ms p99 budget")
        print(f"  {'video':>16}: {cfg.get('video_width')}x{cfg.get('video_height')} "
              f"@{cfg.get('video_fps')} fps")
        print()
        print(f"  {'control sent':>16}: {summary.get('control_messages_sent')}")
        print(f"  {'control recv':>16}: {summary.get('control_messages_received')}")
        print(f"  {'control rate':>16}: {summary.get('control_rate_hz')} Hz "
              "(achieved)")
        print(f"  {'control p50':>16}: {summary.get('control_p50_ms')} ms")
        print(f"  {'control p90':>16}: {summary.get('control_p90_ms')} ms")
        print(f"  {'control p99':>16}: {summary.get('control_p99_ms')} ms")
        print(f"  {'control p99.9':>16}: {summary.get('control_p999_ms')} ms")
        print(f"  {'control max':>16}: {summary.get('control_max_ms')} ms")
        print(f"  {'media sent':>16}: {summary.get('media_sent_datagrams')} "
              f"datagrams")
        print(f"  {'media dropped':>16}: {summary.get('media_dropped_datagrams')} "
              "datagrams")
        print(f"  {'5-tuples':>16}: "
              f"{'distinct' if summary.get('five_tuples_distinct') else 'SHARED'}")
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
                         "<build-dir>/bench_control_saturation.json)")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the summary without writing the artifact")
    ap.add_argument("--capacity-bps", type=int, default=DEFAULT_CAPACITY_BPS,
                    help="the media link's capacity while saturated "
                         "(default: 500000)")
    ap.add_argument("--freq-hz", type=int, default=PROFILE_CONTROL_FREQ_HZ,
                    help="the control plane's rate in Hz (default: 200, "
                         "the profile's)")
    ap.add_argument("--max-p99-ms", type=float,
                    default=PROFILE_CONTROL_MAX_P99_MS,
                    help="the control plane's declared p99 budget "
                         "(default: 50, the profile's)")
    ap.add_argument("--duration-s", type=float, default=20.0,
                    help="the measurement's length, in seconds "
                         "(default: 20)")
    ap.add_argument("--tick-ms", type=int, default=1,
                    help="the engine's tick cadence in ms (default: 1)")
    ap.add_argument("--width", type=int, default=1280,
                    help="the video width (default: 1280)")
    ap.add_argument("--height", type=int, default=720,
                    help="the video height (default: 720)")
    ap.add_argument("--fps", type=int, default=30,
                    help="the video frame rate (default: 30)")
    ap.add_argument("--payload-bytes", type=int, default=32,
                    help="the control message's payload size (default: 32)")
    args = ap.parse_args()

    if args.duration_s <= 0:
        raise SystemExit("error: the duration must be positive")

    profile = load_profile()
    budget, freq = profile_control_budget(profile)
    # The command line wins over the profile: an explicit `--freq-hz`
    # is a deliberate override, and the artifact records both.
    if args.freq_hz != PROFILE_CONTROL_FREQ_HZ:
        freq = float(args.freq_hz)
    if args.max_p99_ms != PROFILE_CONTROL_MAX_P99_MS:
        budget = args.max_p99_ms

    binary = find_binary(args.build_dir)
    if binary is None:
        raise SystemExit(
            f"error: bench_control_saturation not found under {args.build_dir}\n"
            f"       build it first: cmake --build {args.build_dir} "
            f"--target bench_control_saturation")

    scenarios: list[dict] = []
    failures: list[dict] = []

    # Two scenarios: the saturated one (the measurement) and the
    # negative control (the same binary with the video path off, so a
    # passing saturated run cannot be an artefact of the video path
    # having been switched off by accident).
    specs = (
        {
            "name": "saturated",
            "title": "the media link is saturated by a token-bucket bottleneck",
            "with_video": True,
        },
        {
            "name": "negative_control",
            "title": "the same rig with the video path disabled",
            "with_video": False,
        },
    )

    for spec in specs:
        attempt = 0
        while True:
            attempt += 1
            raw = run_benchmark(binary, args, spec["with_video"])
            checks = check_invariants(raw, spec["with_video"], budget, freq)
            # The saturation scenario is retried when the host was too
            # busy to fill the link: a measurement that did not happen
            # is not a result.  The negative control has no load to
            # measure, so it is never retried.
            saturated = (spec["with_video"] and
                         not checks[0]["ok"])
            if not saturated or attempt >= MAX_ATTEMPTS:
                break
            print(f"scenario '{spec['name']}': the host was too busy to "
                  f"saturate the link (attempt {attempt}/{MAX_ATTEMPTS}); "
                  "re-running")

        for c in checks:
            if not c["ok"]:
                failures.append({"scenario": spec["name"], **c})
        scenarios.append({
            "name": spec["name"],
            "title": spec["title"],
            "with_video": spec["with_video"],
            "attempts": attempt,
            "config": raw.get("benchmark", {}),
            "summary": raw.get("summary", {}),
            "endpoints": raw.get("endpoints", {}),
            "invariants": checks,
            # The latency curve: the p99 over the *sorted* sample, so
            # the artifact shows the control plane's latency
            # distribution rather than only its final shape.
            "latency_curve": raw.get("latency_curve", []),
        })
        print(f"ran scenario '{spec['name']}' "
              f"({sum(1 for c in checks if c['ok'])}/{len(checks)} invariants)")

    doc = {
        "schema": 1,
        "benchmark": "control_saturation",
        "generated_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "plan": "docs/plan/v1.2-next-plan.md",
        "plan_item": "E6",
        "evidence_row": ("Remote desktop is zero-copy / the G2 control "
                         "plane is not starved by a saturated video path"),
        "profile": {
            "path": "profiles/remote-desktop.json",
            "transport_strategy": (profile.get("transport") or {}).get("strategy"),
            "control_stack": ((profile.get("transport") or {}).get("control")
                              or {}).get("stack"),
            "control_max_p99_ms": budget,
            "control_freq_hz": freq,
        },
        "binary": str(binary),
        "machine": describe_machine(),
        "scenarios": scenarios,
        "verdict": {
            "all_invariants_hold": not failures,
            "failures": failures,
        },
        "notes": [
            "The two planes are the shipped components: the engine, the "
            "SDP machinery, the control channel's framing, the raw-UDP "
            "ARQ session and the H.264 software encoder. The datagrams "
            "travel over real UDP sockets.",
            "The media plane's saturation is real: the receiving side "
            "enforces a token-bucket capacity and drops the datagrams "
            "that arrive when the bucket is empty, exactly as a "
            "bottleneck router's queue overflows.",
            "The control plane rides the G2 shape: a separate "
            "'raw-udp-arq' stack with its own socket, its own ARQ "
            "session and its own 5-tuple. Its address travels in the "
            "session description ('a=x-nimrtc-control:'), so the two "
            "planes' endpoints are really distinct rather than merely "
            "asserted to be.",
            "The control producer, the media producer and the engine "
            "tick each run on their own thread. That is the point of "
            "G2, not a convenience: driving both planes from one loop "
            "would put the software H.264 encode on the control plane's "
            "critical path, and the measurement would report the "
            "encoder's latency rather than the transport's.",
            "The numbers are machine-dependent (the host's real "
            "scheduling jitter, the loopback's real RTT), so the "
            "committed artifact is one local sample, not a pass/fail "
            "threshold. What must hold on every machine are the "
            "invariants, and they are asserted by "
            "tools/benchmark_control_saturation.py and recorded in the "
            "artifact.",
            "The 'negative_control' scenario runs the same rig with the "
            "video path disabled: it proves that a passing saturated "
            "run is not an artefact of the video path having been "
            "switched off by accident, and that the control path "
            "delivers every message on an idle link too.",
            "The control producer's schedule is absolute: a late wake "
            "is absorbed by the next slot rather than by shifting every "
            "slot that follows. The obvious alternative — resetting the "
            "phase to 'now + period' — makes the measured period the "
            "wake error plus the configured period, so a 500 us wake "
            "error turns a 5 ms period into a 5.5 ms one and the "
            "control plane reports ~182 Hz instead of the profile's "
            "200 Hz. On Windows the wait is a high-resolution waitable "
            "timer (CREATE_WAITABLE_TIMER_HIGH_RESOLUTION), because the "
            "default ~15.6 ms system tick would cap the producer at "
            "~64 Hz and the measured rate would describe the host's "
            "timer rather than the transport.",
        ],
    }

    if not args.dry_run:
        out = args.out or (args.build_dir / "bench_control_saturation.json")
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
