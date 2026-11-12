#!/usr/bin/env python3
# Copyright (c) 2026 NimRTC contributors.
# SPDX-License-Identifier: MIT
"""Capture and compare the Phase 0.3 hot-path baseline.

The architecture remediation plan (§0.3) asks for three baseline points
before Phase 3 starts optimising the hot paths:

  1. `send_video()` at 1080p30 — the per-call heap allocations and the
     CPU cost.
  2. `tick()` — the p99 duration.
  3. The scheduler under load — `drain_with()`'s cost and how long a
     concurrent `enqueue()` has to wait for the lock.

`tests/bench_engine_hotpath.cpp` measures them and prints a JSON
document.  This wrapper runs it, writes the document into `build/`, and
can diff two runs so "Phase 3 made it faster" is a number rather than
an impression.

    # Capture a run.
    python tools/benchmark_hotpath.py --build-dir build

    # Capture a run and compare it against a previous one.
    python tools/benchmark_hotpath.py --build-dir build \\
        --baseline build/bench_hotpath_baseline.json

    # Compare two existing captures without running anything.
    python tools/benchmark_hotpath.py --compare \\
        build/bench_hotpath_baseline.json build/bench_hotpath_phase3.json

Output goes into `build/`, never into the project root (see
`.cursor/rules/root-cleanliness.mdc`).
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# The metrics whose direction is "smaller is better".  A regression is a
# value that grew.
LOWER_IS_BETTER = (
    ("send_video", "allocations_per_frame"),
    ("send_video", "bytes_per_frame"),
    ("send_video", "mean_us"),
    ("send_video", "p99_us"),
    ("send_video", "mib_per_second"),
    ("tick", "mean_us"),
    ("tick", "p99_us"),
    ("tick", "max_us"),
    ("scheduler", "enqueue_mean_us"),
    ("scheduler", "enqueue_p99_us"),
    ("scheduler", "enqueue_max_us"),
    ("scheduler", "drain_mean_us"),
    ("scheduler", "drain_p99_us"),
    ("scheduler", "drain_max_us"),
)


def find_binary(build_dir: Path) -> Path | None:
    """Locate `bench_engine_hotpath` in a build tree.

    The multi-config generators (MSVC) put it under `<config>/`, the
    single-config ones (Ninja / Makefiles) directly under `tests/`.
    """
    tests = build_dir / "tests"
    candidates = [
        tests / "bench_engine_hotpath",
        tests / "Debug" / "bench_engine_hotpath.exe",
        tests / "Release" / "bench_engine_hotpath.exe",
        tests / "RelWithDebInfo" / "bench_engine_hotpath.exe",
        tests / "MinSizeRel" / "bench_engine_hotpath.exe",
    ]
    for c in candidates:
        if c.is_file():
            return c
    return None


def run_benchmark(binary: Path, args: argparse.Namespace) -> dict:
    cmd = [
        str(binary),
        "--width", str(args.width),
        "--height", str(args.height),
        "--frames", str(args.frames),
        "--ticks", str(args.ticks),
        "--packets", str(args.packets),
    ]
    # The engine's log lines go to stderr and may carry characters the
    # host's default code page cannot represent (the Chinese locale's
    # GBK, for one).  Decoding them strictly would abort the capture on
    # a log line, so they are decoded leniently and then discarded —
    # only stdout carries the JSON document.
    proc = subprocess.run(cmd, capture_output=True, text=True,
                          encoding="utf-8", errors="replace")
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        raise SystemExit(f"error: the benchmark exited with {proc.returncode}")
    # The benchmark prints exactly one JSON document on stdout; the
    # engine's log lines go to stderr.
    for line in reversed(proc.stdout.strip().splitlines()):
        line = line.strip()
        if line.startswith("{"):
            return json.loads(line)
    raise SystemExit("error: the benchmark printed no JSON document")


def metric(doc: dict, section: str, name: str):
    return doc.get(section, {}).get(name)


def compare(base: dict, new: dict, threshold: float) -> list[str]:
    """Return one line per metric that moved by more than `threshold`."""
    lines = []
    # The allocation metrics only mean something when both runs had the
    # probe enabled.  A sanitizer build reports zeros, which would show
    # up as a spectacular "improvement" that never happened.
    probe_on = (base.get("benchmark", {}).get("alloc_probe_enabled", True)
                and new.get("benchmark", {}).get("alloc_probe_enabled", True))
    for section, name in LOWER_IS_BETTER:
        if not probe_on and name in ("allocations_per_frame", "bytes_per_frame"):
            continue
        b = metric(base, section, name)
        n = metric(new, section, name)
        if not isinstance(b, (int, float)) or not isinstance(n, (int, float)):
            continue
        if b == 0:
            continue
        delta = (n - b) / b * 100.0
        if abs(delta) < threshold:
            continue
        direction = "worse" if delta > 0 else "better"
        lines.append(f"  {section}.{name}: {b:.4f} -> {n:.4f} "
                     f"({delta:+.1f}%, {direction})")
    return lines


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", type=Path, default=ROOT / "build",
                    help="the build tree that holds the benchmark (default: build/)")
    ap.add_argument("--out", type=Path, default=None,
                    help="where to write the capture (default: <build-dir>/bench_hotpath.json)")
    ap.add_argument("--baseline", type=Path, default=None,
                    help="a previous capture to compare against")
    ap.add_argument("--compare", nargs=2, type=Path, metavar=("A", "B"),
                    help="compare two existing captures and exit")
    ap.add_argument("--width", type=int, default=1920)
    ap.add_argument("--height", type=int, default=1080)
    ap.add_argument("--frames", type=int, default=300)
    ap.add_argument("--ticks", type=int, default=2000)
    ap.add_argument("--packets", type=int, default=20000)
    ap.add_argument("--threshold", type=float, default=5.0,
                    help="the percentage change that counts as a move (default: 5)")
    args = ap.parse_args()

    if args.compare:
        base = json.loads(args.compare[0].read_text(encoding="utf-8"))
        new = json.loads(args.compare[1].read_text(encoding="utf-8"))
        print(f"baseline: {args.compare[0]}")
        print(f"current : {args.compare[1]}")
        lines = compare(base, new, args.threshold)
        if not lines:
            print(f"no metric moved by more than {args.threshold:.1f}%")
            return 0
        print("moved metrics:")
        print("\n".join(lines))
        return 0

    binary = find_binary(args.build_dir)
    if binary is None:
        raise SystemExit(
            f"error: bench_engine_hotpath not found under {args.build_dir}\n"
            f"       build it first: cmake --build {args.build_dir} --target bench_engine_hotpath")

    doc = run_benchmark(binary, args)

    # A sanitizer build cannot report the allocation counts — the
    # sanitizer runtime owns `operator new` — so the benchmark says so
    # explicitly.  Say it here too, rather than printing a misleading
    # "0.00 allocs/frame".
    probe_on = doc.get("benchmark", {}).get("alloc_probe_enabled", True)

    out = args.out or (args.build_dir / "bench_hotpath.json")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {out}")

    sv = doc.get("send_video", {})
    tk = doc.get("tick", {})
    sc = doc.get("scheduler", {})
    if probe_on:
        print(f"  send_video : {sv.get('allocations_per_frame', 0):.2f} allocs/frame, "
              f"{sv.get('bytes_per_frame', 0) / (1024 * 1024):.2f} MiB/frame, "
              f"p99 {sv.get('p99_us', 0):.1f} us")
    else:
        print(f"  send_video : {sv.get('bytes_per_frame', 0) / (1024 * 1024):.2f} MiB/frame, "
              f"p99 {sv.get('p99_us', 0):.1f} us "
              f"(the allocation probe is off — this is a sanitizer build)")
    print(f"  tick       : p99 {tk.get('p99_us', 0):.2f} us, "
          f"max {tk.get('max_us', 0):.2f} us")
    print(f"  scheduler  : enqueue p99 {sc.get('enqueue_p99_us', 0):.1f} us, "
          f"drain p99 {sc.get('drain_p99_us', 0):.1f} us")

    if args.baseline:
        base = json.loads(args.baseline.read_text(encoding="utf-8"))
        print(f"\nagainst {args.baseline}:")
        lines = compare(base, doc, args.threshold)
        if not lines:
            print(f"  no metric moved by more than {args.threshold:.1f}%")
        else:
            print("\n".join(lines))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
