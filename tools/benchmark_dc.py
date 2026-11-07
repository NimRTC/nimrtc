#!/usr/bin/env python3
# Copyright (c) 2026 NimRTC contributors.
# SPDX-License-Identifier: MIT
"""Measure localhost UDP loopback throughput baseline (Track A, A2).

This is the transport-floor baseline for the v1.1 quality release, not a
claim about WebTransport/QUIC end-to-end throughput. It bursts N UDP
datagrams over 127.0.0.1 per payload size, drains them in-process, and
reports pps + Mbps per size.

Payload sizes model the two DataChannel planes (`IDataChannel` QoS,
`WebTransportDataChannel` routing):
  - 256 / 1200 B  -> datagram plane (unreliable DC path; QUIC DATAGRAM
    frames top out near ~1200 B).
  - 16384 B       -> bidi-stream plane (reliable DC path chunk size).

Output goes to build/e2e/bench_dc.json so the project root stays
clean. Re-run with:

    python tools/benchmark_dc.py
    python tools/benchmark_dc.py --count 2000 --sizes 256,1200,16384
    python tools/benchmark_dc.py --json
"""

from __future__ import annotations

import argparse
import json
import socket
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
E2E = ROOT / "build" / "e2e"

DEFAULT_SIZES = (256, 1200, 16384)

# The credit window the large-payload plane sends behind.  It bounds how
# far the sender may run ahead of the receiver, which is what keeps the
# loopback UDP buffer from overflowing (and dropping datagrams) without
# the fixed sleep that used to distort the measurement.
CREDIT_WINDOW = 64

# How long the sender waits between two credit checks.  This is a backoff
# between progress probes, not the pacing interval itself.
CREDIT_POLL_S = 0.0002

# How long the sender waits for the last datagrams to be drained before it
# stops the clock.
DRAIN_TIMEOUT_S = 10.0


def plane_for(size: int) -> str:
    if size <= 1200:
        return "datagram"
    return "bidi-stream-chunk"


def run_throughput(count: int, size: int) -> dict:
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    # Large loopback bursts need room; 4 MB each way where supported.
    for s in (rx, tx):
        try:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 << 20)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
        except OSError:
            pass
    rx.bind(("127.0.0.1", 0))
    port = rx.getsockname()[1]
    # A short socket timeout, so the receiver can re-check the overall
    # deadline instead of blocking in recvfrom until the very end.
    rx.settimeout(0.25)
    payload = bytes((i % 251 for i in range(size)))
    received = 0
    total_bytes = 0
    stop = threading.Event()
    # The overall budget for the whole exchange: the send burst plus the
    # drain of whatever is still in flight.
    deadline = time.monotonic() + DRAIN_TIMEOUT_S

    def recv_loop() -> None:
        nonlocal received, total_bytes
        while received < count and not stop.is_set():
            try:
                data, _ = rx.recvfrom(65535)
            except socket.timeout:
                # Keep waiting until the overall deadline passes; a timeout
                # is only a chance to re-check it, not an end condition.
                if time.monotonic() > deadline:
                    break
                continue
            except OSError:
                break
            received += 1
            total_bytes += len(data)

    worker = threading.Thread(target=recv_loop, daemon=True)
    worker.start()
    time.sleep(0.05)  # let the receiver block in recvfrom first
    t0 = time.perf_counter_ns()
    # Large-payload bursts (16 KB x 2000 = ~32 MB back-to-back) overflow the
    # loopback UDP buffers on shared CI runners, causing flaky drops, so the
    # sender has to stay behind the receiver.
    #
    # The pacing is a CREDIT WINDOW, not a fixed sleep: the sender waits
    # until the receiver has drained back to within `window` datagrams of
    # the send position.  A fixed sleep cannot do this job -- on Windows
    # the default timer resolution is ~15.6 ms, so a `time.sleep(0.002)`
    # costs ~15 ms of wall clock and the benchmark ends up measuring the
    # SLEEPER instead of the transport (that is how the 16384 B row came
    # to report ~3.1k pps / ~410 Mbps while the README claimed ~70k pps /
    # ~9 Gbps).  The credit window adapts to the receiver's actual drain
    # rate, so it neither drops datagrams nor caps the measurement.
    window = CREDIT_WINDOW
    for i in range(count):
        tx.sendto(payload, ("127.0.0.1", port))
        # Let the receiver catch up until it is at most `window`
        # datagrams behind.  The sleep is a backoff between progress
        # probes, not the pacing interval itself: the loop exits as soon
        # as the receiver has drained.
        while received < i - window and not stop.is_set():
            time.sleep(CREDIT_POLL_S)
    # Drain whatever is still in flight so the timing covers the whole
    # batch rather than just the send loop.  The receiver keeps running
    # during this wait -- stopping it here would discard the datagrams
    # that are already in the socket buffer.
    while received < count and time.monotonic() < deadline:
        time.sleep(0.001)
    t1 = time.perf_counter_ns()
    stop.set()
    worker.join(timeout=1.0)
    rx.close()
    tx.close()

    elapsed_s = (t1 - t0) / 1e9
    pps = received / elapsed_s if elapsed_s > 0 else 0.0
    mbps = (total_bytes * 8) / elapsed_s / 1e6 if elapsed_s > 0 else 0.0
    return {
        "plane": plane_for(size),
        "payload_bytes": size,
        "sent": count,
        "received": received,
        "total_bytes": total_bytes,
        "elapsed_s": elapsed_s,
        "pps": pps,
        "mbps": mbps,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description="UDP loopback DC throughput baseline.")
    ap.add_argument("--count", type=int, default=2000,
                    help="datagrams per payload size (default: 2000)")
    ap.add_argument("--sizes", type=str,
                    default=",".join(str(s) for s in DEFAULT_SIZES),
                    help="comma-separated payload sizes in bytes")
    ap.add_argument("--json", action="store_true",
                    help="print JSON instead of human-readable text")
    args = ap.parse_args()

    sizes = [int(s) for s in args.sizes.split(",") if s.strip()]
    if not sizes:
        print("error: --sizes must list at least one payload size")
        return 1

    results = [run_throughput(args.count, size) for size in sizes]
    result = {
        "count_per_size": args.count,
        "results": results,
        "note": ("localhost UDP loopback transport floor; not "
                 "WebTransport/QUIC end-to-end; 'datagram' rows model the "
                 "unreliable DC path, 'bidi-stream-chunk' rows model the "
                 "reliable DC path"),
    }
    E2E.mkdir(parents=True, exist_ok=True)
    out = E2E / "bench_dc.json"
    out.write_text(json.dumps(result, indent=2), encoding="utf-8")

    if args.json:
        print(json.dumps(result, indent=2))
    else:
        for r in results:
            print(f"[{r['plane']}] size={r['payload_bytes']}B "
                  f"rx={r['received']}/{r['sent']} "
                  f"pps={r['pps']:.0f} mbps={r['mbps']:.1f}")
        print(f"wrote {out}")
    ok = all(r["received"] == args.count for r in results)
    return 0 if ok else 2


if __name__ == "__main__":
    raise SystemExit(main())
