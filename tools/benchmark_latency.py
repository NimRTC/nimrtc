#!/usr/bin/env python3
# Copyright (c) 2026 NimRTC contributors.
# SPDX-License-Identifier: MIT
"""Measure localhost UDP loopback latency/jitter baseline (Track A).

This is the transport-floor baseline for the v1.1 quality release, not a
claim about full engine glass-to-glass latency. It sends N UDP datagrams
over 127.0.0.1, echoes them in-process, and reports p50/p99/max + jitter.

Output goes to build/e2e/bench_latency.json so the project root stays
clean. Re-run with:

    python tools/benchmark_latency.py --count 1000 --size 172
    python tools/benchmark_latency.py --json

172 bytes ~= 20ms Opus mono @48kHz (160 samples int16) + RTP header.
"""

from __future__ import annotations

import argparse
import json
import socket
import statistics
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
E2E = ROOT / "build" / "e2e"


def run_loopback(count: int, size: int) -> dict:
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.bind(("127.0.0.1", 0))
    port = rx.getsockname()[1]
    rx.settimeout(2.0)
    payload = bytes((i % 251 for i in range(size)))
    samples_us: list[float] = []
    for _ in range(count):
        t0 = time.perf_counter_ns()
        tx.sendto(payload, ("127.0.0.1", port))
        data, _ = rx.recvfrom(65535)
        t1 = time.perf_counter_ns()
        assert len(data) == size
        samples_us.append((t1 - t0) / 1000.0)
    rx.close()
    tx.close()
    samples_us.sort()
    n = len(samples_us)

    def pct(q: float) -> float:
        if n == 0:
            return 0.0
        idx = min(n - 1, int(q * n))
        return samples_us[idx]

    diffs = [abs(samples_us[i + 1] - samples_us[i]) for i in range(n - 1)]
    return {
        "count": n,
        "payload_bytes": size,
        "p50_us": pct(0.50),
        "p99_us": pct(0.99),
        "max_us": samples_us[-1] if n else 0.0,
        "mean_us": statistics.fmean(samples_us) if n else 0.0,
        "mean_jitter_us": statistics.fmean(diffs) if diffs else 0.0,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description="UDP loopback latency baseline.")
    ap.add_argument("--count", type=int, default=1000)
    ap.add_argument("--size", type=int, default=172)
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    result = run_loopback(args.count, args.size)
    result["note"] = "localhost UDP echo floor; not engine glass-to-glass"
    E2E.mkdir(parents=True, exist_ok=True)
    out = E2E / "bench_latency.json"
    out.write_text(json.dumps(result, indent=2), encoding="utf-8")

    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print(f"count={result['count']} size={result['payload_bytes']}B")
        print(f"p50={result['p50_us']:.1f}us p99={result['p99_us']:.1f}us "
              f"max={result['max_us']:.1f}us mean_jitter={result['mean_jitter_us']:.1f}us")
        print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
