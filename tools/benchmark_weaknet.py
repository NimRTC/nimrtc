#!/usr/bin/env python3
# Copyright (c) 2026 NimRTC contributors.
# SPDX-License-Identifier: MIT
"""Portable weak-network baseline (v1.3 Q4).

Why in-process instead of `tc qdisc netem`: netem is Linux-only and needs
root + a real interface. CI runs on Windows/macOS/Linux and the bench must
be reproducible on a laptop with no privileges. So this tool puts a small
userspace shaper between two loopback UDP sockets and applies the three
v1.3-plan Q4 tiers deterministically (seeded RNG):

  - loss-5pct    : 5% uniform drop, 5 ms base delay
  - jitter-100ms : 20 ms base + uniform 0..100 ms jitter
  - cap-1mbps    : 1 Mbps token-pace, 5 ms base delay

A rate-limited link delays a packet by its base delay *plus* the time
the link needs to put the packet on the wire (its serialization time),
and it only does so without building a queue if the sender stays below
the capacity.  The capped tier therefore paces its sends at `--util`
(default 0.9) of the cap and adds the serialization time to the
packet's delay.

Pipeline per tier (four threads, all localhost UDP):

  TX --(paced sends)--> shaper recv --(heap by deliver_at)--> forwarder
      --(per-packet delay)--> RX drain (timestamps arrivals concurrently)

Latency is arrival minus send per sequence number, so queued packets do
not inflate each other's readings: the RX drain runs concurrently with
the sends, and the forwarder serves the delay heap in deliver_at order
(one sleeper, no head-of-line blocking, no per-packet thread).

Output goes to build/e2e/bench_weaknet.json so the project root stays
clean. Reproduce with:
    python tools/benchmark_weaknet.py --count 300
    python tools/benchmark_weaknet.py --count 300 --json

Linux parity: `--mode netem` hands the impairments to the kernel instead
of the in-process shaper. The impairments then come from a `tc qdisc
netem` qdisc that the caller attaches to the sending interface (see
tools/benchmark_weaknet_netem.sh, which builds a veth pair and drives
this mode). In that mode the tool only paces the sends and measures
arrival-minus-send; it never drops or delays a packet itself.
"""

from __future__ import annotations

import argparse
import heapq
import json
import random
import socket
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
E2E = ROOT / "build" / "e2e"


def _high_res_timer():
    """Context: 1 ms timer resolution on Windows (default is ~15.6 ms).

    Without this, every Condition.wait(timeout<15 ms) oversleeps to ~15 ms
    and the 5 ms base-delay tier reports p50 ~15 ms. timeBeginPeriod(1)
    makes short sleeps/waits accurate to ~1-2 ms. No-op off Windows.
    Returns a callable restoring the default resolution.
    """
    try:
        import ctypes

        winmm = ctypes.windll.winmm
        # TIMERR_NOERROR == 0 means the resolution is now 1 ms; keep it
        # until restore_timer() runs. (An earlier revision called
        # timeEndPeriod immediately here, cancelling the request.)
        if winmm.timeBeginPeriod(1) == 0:
            return lambda: winmm.timeEndPeriod(1)
        return lambda: None
    except Exception:
        return lambda: None

TIERS = (
    {"id": "loss-5pct", "loss": 0.05, "base_ms": 5.0, "jitter_ms": 0.0, "cap_mbps": 0.0},
    {"id": "jitter-100ms", "loss": 0.0, "base_ms": 20.0, "jitter_ms": 100.0, "cap_mbps": 0.0},
    {"id": "cap-1mbps", "loss": 0.0, "base_ms": 5.0, "jitter_ms": 0.0, "cap_mbps": 1.0},
)

# ---------------------------------------------------------------------------
# Wire accounting for the capped tier.
#
# A link at a fixed rate does not delay a packet by "the delay"; it
# delays it by the time the link needs to put the packet on the wire
# (its serialization time) plus whatever is already queued ahead of it.
# Both implementations below therefore work in *wire* bytes, not
# payload bytes, and both keep the offered load a little below the
# capacity so the queue stays empty and the reading is the link's
# delay rather than self-inflicted backlog.
#
# The two modes differ in what "wire bytes" means:
#
#   * The portable shaper charges the payload only: the sender paces
#     the packets and the shaper adds the link's serialization time
#     to each packet's delay.
#   * netem charges the *on-wire* length of the datagram (skb->len),
#     which over an IPv4 loopback is the UDP payload plus the UDP,
#     IP and loopback-Ethernet headers.  netem additionally rounds
#     that length up to the qdisc's cell size, which
#     tools/benchmark_weaknet_netem.sh pins to 1 byte so the charge
#     is the exact byte count.
# ---------------------------------------------------------------------------

SEQ_BYTES = 4  # the per-packet sequence number the tool prepends
NETEM_WIRE_OVERHEAD = 8 + 20 + 14  # UDP + IPv4 + loopback Ethernet
UTIL_DEFAULT = 0.9  # offered load as a fraction of the capacity
FORWARD_SLICE_S = 0.0005  # forwarder's delivery-deadline polling slice


def _serialization_s(tier: dict, size: int) -> float:
    """Seconds the link needs to put one packet of `size` payload on the wire.

    Zero for the uncapped tiers: they have no rate limit, so there is
    no serialization time to account for.
    """
    if tier["cap_mbps"] <= 0:
        return 0.0
    return (size * 8) / (tier["cap_mbps"] * 1e6)


def _netem_charged_bytes(size: int) -> int:
    """Bytes netem's rate limiter charges for one of our datagrams.

    netem charges the datagram's on-wire length (skb->len), rounded up
    to the qdisc's cell size.  The script pins the cell size to 1 byte
    ("rate 1mbit 0 1"), so the charge is the on-wire length: our
    sequence number plus the payload, plus the UDP, IPv4 and
    loopback-Ethernet headers the kernel adds on the way out.
    """
    return size + SEQ_BYTES + NETEM_WIRE_OVERHEAD


def _pct(sorted_us: list[float], q: float) -> float:
    if not sorted_us:
        return 0.0
    idx = min(len(sorted_us) - 1, int(q * len(sorted_us)))
    return sorted_us[idx]


def _pace_schedule(
    tier: dict,
    count: int,
    size: int,
    util: float = UTIL_DEFAULT,
    charged_bytes: int | None = None,
) -> list[float]:
    """Earliest send instant per packet, relative to the run start.

    The capped tier is paced at `util` times the link's capacity, in
    the bytes the link actually charges, so the link's queue stays
    empty: the reading is then the link's delay, not the queue depth a
    faster sender would build up.  The uncapped tiers use a fixed 2 ms
    cadence (~500 pps) for the same reason — without pacing, TX bursts
    all N packets at once and the serial shaper / forwarder backlog
    (~50 us/packet of Python + syscall) dominates the reading, so the
    5 ms tier would report ~25 ms of queue instead of the configured
    5 ms delay.

    `charged_bytes` is the byte count the link charges per packet: the
    payload for the portable shaper (which accounts in payload bytes),
    the on-wire length for netem (which accounts in skb bytes).  When
    it is None the payload size is used.
    """
    pace_s = [0.0] * count
    if tier["cap_mbps"] > 0:
        bytes_per_pkt = charged_bytes if charged_bytes is not None else size
        per_pkt = (bytes_per_pkt * 8) / (tier["cap_mbps"] * 1e6) / util
        for i in range(1, count):
            pace_s[i] = pace_s[i - 1] + per_pkt
    else:
        for i in range(1, count):
            pace_s[i] = pace_s[i - 1] + 0.002
    return pace_s


def run_netem_tier(
    tier: dict,
    count: int,
    size: int,
    host: str = "127.0.0.1",
    util: float = UTIL_DEFAULT,
) -> dict:
    """Measure one tier whose impairments are applied by the kernel.

    In this mode the tool drops nothing and delays nothing: the
    `tc qdisc netem` qdisc that the caller attached to the interface
    does both.  The tool only paces the sends (at the tier's rate, so
    the netem rate limiter is a ceiling rather than a queue-builder)
    and measures arrival-minus-send per sequence number.

    Because the loss is applied by the kernel, the number of survivors
    is not known in advance: the run ends when the link goes quiet for
    a tail window instead of when a precomputed count is reached.
    """
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    for s in (rx, tx):
        try:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 << 20)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
        except OSError:
            pass
    rx.bind((host, 0))
    tx.bind((host, 0))
    rx_port = rx.getsockname()[1]
    rx.settimeout(0.2)
    payload = bytes((i % 251 for i in range(size)))
    charged = _netem_charged_bytes(size)
    pace_s = _pace_schedule(tier, count, size, util, charged_bytes=charged)

    send_ns: dict[int, int] = {}
    arrival_ns: dict[int, int] = {}
    arr_mu = threading.Lock()
    stop = threading.Event()

    def rx_drain() -> None:
        last_arrival = time.perf_counter()
        try:
            while not stop.is_set():
                try:
                    data, _ = rx.recvfrom(65535)
                except socket.timeout:
                    # A quiet tail means the kernel-side impairments have
                    # finished delivering; the loopback is not going to
                    # produce anything else.
                    with arr_mu:
                        got = len(arrival_ns)
                    if got >= count or (time.perf_counter() - last_arrival) > 2.0:
                        return
                    continue
                if len(data) < 4:
                    continue
                seq = int.from_bytes(data[:4], "big")
                last_arrival = time.perf_counter()
                with arr_mu:
                    if seq not in arrival_ns:
                        arrival_ns[seq] = time.perf_counter_ns()
                    if len(arrival_ns) >= count:
                        return
        except OSError:
            pass

    t_drain = threading.Thread(target=rx_drain, daemon=True)
    t_drain.start()
    time.sleep(0.05)

    t_start = time.perf_counter_ns()
    for seq in range(count):
        if pace_s[seq] > 0:
            # The schedule is an absolute offset from the start of the
            # run, not a delta: adding the accumulated offset to the
            # *current* time would make each packet's target drift
            # further out than the last, and the send rate would
            # diverge instead of holding at the tier's rate.
            target = t_start + int(pace_s[seq] * 1e9)
            while time.perf_counter_ns() < target:
                time.sleep(0.001)
        send_ns[seq] = time.perf_counter_ns()
        try:
            tx.sendto(seq.to_bytes(4, "big") + payload, (host, rx_port))
        except OSError:
            break

    # Let the tail drain: the last packet may still be sitting in the
    # netem qdisc's delay queue.
    deadline = time.time() + 10.0
    while time.time() < deadline:
        with arr_mu:
            got = len(arrival_ns)
        if got >= count:
            break
        time.sleep(0.05)
    stop.set()
    t_drain.join(timeout=3.0)
    rx.close()
    tx.close()

    lat_us = [
        (arrival_ns[s] - send_ns[s]) / 1000.0 for s in arrival_ns if s in send_ns
    ]
    lat_us.sort()
    received = len(arrival_ns)
    # The kernel dropped whatever did not arrive, so the delivery ratio
    # is the measurement and the expected count is the sent count.
    return {
        "tier": tier["id"],
        "loss_config": tier["loss"],
        "base_ms": tier["base_ms"],
        "jitter_ms": tier["jitter_ms"],
        "cap_mbps": tier["cap_mbps"],
        "sent": count,
        "received": received,
        "expected": count,
        "util": util,
        "charged_bytes": charged,
        "serialization_us": (
            (charged * 8) / (tier["cap_mbps"] * 1e6) * 1e6
            if tier["cap_mbps"] > 0
            else 0.0
        ),
        "delivery_ratio": (received / count) if count else 0.0,
        "p50_us": _pct(lat_us, 0.50),
        "p99_us": _pct(lat_us, 0.99),
        "max_us": lat_us[-1] if lat_us else 0.0,
    }


def run_tier(
    tier: dict,
    count: int,
    size: int,
    seed: int,
    util: float = UTIL_DEFAULT,
) -> dict:
    rng = random.Random(seed)
    # Pre-roll every packet decision so the run is deterministic and no
    # worker thread does RNG under load.
    drop = [rng.random() < tier["loss"] for _ in range(count)]
    # A rate-limited link delays a packet by the time it needs to put
    # the packet on the wire (its serialization time) plus the
    # configured base delay, so the capped tier's delay carries that
    # serialization term.  The uncapped tiers have no rate, so their
    # serialization term is zero.
    ser_s = _serialization_s(tier, size)
    delay_s = [
        (tier["base_ms"] + rng.random() * tier["jitter_ms"]) / 1000.0 + ser_s
        for _ in range(count)
    ]
    # Token pace for the capped tier: earliest send instant per packet.
    pace_s = _pace_schedule(tier, count, size, util)

    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sh = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    for s in (rx, sh, tx):
        try:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 << 20)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
        except OSError:
            pass
    rx.bind(("127.0.0.1", 0))
    sh.bind(("127.0.0.1", 0))
    rx_port = rx.getsockname()[1]
    sh_port = sh.getsockname()[1]
    rx.settimeout(2.0)
    sh.settimeout(2.0)
    payload = bytes((i % 251 for i in range(size)))
    stop = threading.Event()

    heap: list[tuple[float, bytes]] = []
    heap_mu = threading.Lock()
    heap_cv = threading.Condition(heap_mu)
    send_ns: dict[int, int] = {}
    arrival_ns: dict[int, int] = {}
    arr_mu = threading.Lock()
    expected = sum(1 for d in drop if not d)

    def shaper_recv() -> None:
        # Pull TX datagrams, push survivors onto the delay heap.
        got = 0
        try:
            while got < count and not stop.is_set():
                try:
                    data, _ = sh.recvfrom(65535)
                except socket.timeout:
                    continue
                got += 1
                if len(data) < 4:
                    continue
                seq = int.from_bytes(data[:4], "big")
                if 0 <= seq < count and not drop[seq]:
                    with heap_cv:
                        # perf_counter, not monotonic: on Windows
                        # time.monotonic() is GetTickCount64, whose
                        # resolution is 15.6 ms, so scheduling on it
                        # would quantize every packet's delay to a
                        # 15.6 ms step.  perf_counter is
                        # QueryPerformanceCounter (1e-7 s).
                        heapq.heappush(heap, (time.perf_counter() + delay_s[seq], data))
                        heap_cv.notify()
        except OSError:
            pass
        with heap_cv:
            heap_cv.notify()

    def forwarder() -> None:
        # Serve the heap in deliver_at order; one sleeper, no
        # head-of-line blocking, no per-packet thread.
        #
        # The wait for a packet's delivery time does *not* use
        # Condition.wait(timeout=...): on Windows a timed Condition
        # wait expires on a ~10 ms granularity (measured: a 10 ms wait
        # returns after ~20 ms), which would add that much jitter to
        # every reading.  Instead the forwarder sleeps in short
        # slices outside the lock and re-checks the heap, so a packet
        # that arrives with an earlier deliver_at (possible in the
        # jitter tier, where the per-packet delay varies) is picked up
        # within a slice, and the delivery deadline is met to within
        # about a millisecond.
        try:
            while not stop.is_set():
                with heap_cv:
                    while not heap and not stop.is_set():
                        heap_cv.wait(timeout=0.5)
                    if stop.is_set():
                        return
                    if not heap:
                        continue
                    deliver_at = heap[0][0]
                now = time.perf_counter()
                wait = deliver_at - now
                if wait > 0:
                    time.sleep(min(wait, FORWARD_SLICE_S))
                    continue
                with heap_cv:
                    if not heap:
                        continue
                    deliver_at = heap[0][0]
                    if deliver_at > time.perf_counter():
                        continue
                    _, data = heapq.heappop(heap)
                try:
                    rx.sendto(data, ("127.0.0.1", rx_port))
                except OSError:
                    return
        except OSError:
            pass

    def rx_drain() -> None:
        try:
            while not stop.is_set():
                try:
                    data, _ = rx.recvfrom(65535)
                except socket.timeout:
                    with arr_mu:
                        done = len(arrival_ns)
                    if done >= expected:
                        return
                    continue
                if len(data) < 4:
                    continue
                seq = int.from_bytes(data[:4], "big")
                with arr_mu:
                    if seq not in arrival_ns:
                        arrival_ns[seq] = time.perf_counter_ns()
                    if len(arrival_ns) >= expected:
                        return
        except OSError:
            pass

    t_recv = threading.Thread(target=shaper_recv, daemon=True)
    t_fwd = threading.Thread(target=forwarder, daemon=True)
    t_drain = threading.Thread(target=rx_drain, daemon=True)
    t_recv.start()
    t_fwd.start()
    t_drain.start()
    time.sleep(0.05)

    t_start = time.perf_counter_ns()
    for seq in range(count):
        if pace_s[seq] > 0:
            target = t_start + int(pace_s[seq] * 1e9)
            while time.perf_counter_ns() < target:
                time.sleep(0.001)
        send_ns[seq] = time.perf_counter_ns()
        tx.sendto(seq.to_bytes(4, "big") + payload, ("127.0.0.1", sh_port))

    # Wait for the survivors (or a bounded drain tail: max configured
    # delay + margin), then stop the workers.
    max_delay = max(delay_s) if delay_s else 0.0
    deadline = time.time() + max_delay + 10.0
    while time.time() < deadline:
        with arr_mu:
            if len(arrival_ns) >= expected:
                break
        time.sleep(0.02)
    stop.set()
    with heap_cv:
        heap_cv.notify()
    t_recv.join(timeout=3.0)
    t_fwd.join(timeout=3.0)
    t_drain.join(timeout=3.0)
    rx.close()
    sh.close()
    tx.close()

    lat_us = [
        (arrival_ns[s] - send_ns[s]) / 1000.0 for s in arrival_ns if s in send_ns
    ]
    lat_us.sort()
    return {
        "tier": tier["id"],
        "loss_config": tier["loss"],
        "base_ms": tier["base_ms"],
        "jitter_ms": tier["jitter_ms"],
        "cap_mbps": tier["cap_mbps"],
        "sent": count,
        "received": len(arrival_ns),
        "expected": expected,
        "util": util,
        "charged_bytes": size,
        "serialization_us": ser_s * 1e6,
        "delivery_ratio": (len(arrival_ns) / count) if count else 0.0,
        "p50_us": _pct(lat_us, 0.50),
        "p99_us": _pct(lat_us, 0.99),
        "max_us": lat_us[-1] if lat_us else 0.0,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description="Portable weak-net baseline (v1.3 Q4).")
    ap.add_argument("--count", type=int, default=300)
    ap.add_argument("--size", type=int, default=1200)
    ap.add_argument("--seed", type=int, default=1234)
    ap.add_argument("--json", action="store_true")
    ap.add_argument(
        "--mode",
        choices=("shaper", "netem"),
        default="shaper",
        help=(
            "shaper: impairments are applied by the in-process userspace "
            "shaper (portable, no privileges).  netem: impairments are "
            "applied by a 'tc qdisc netem' qdisc that the caller attached "
            "to the interface; the tool only paces sends and measures."
        ),
    )
    ap.add_argument(
        "--tier",
        action="append",
        choices=[t["id"] for t in TIERS],
        default=None,
        help=(
            "restrict the run to the given tier(s); repeatable.  Used by "
            "tools/benchmark_weaknet_netem.sh, which has to attach one "
            "qdisc per tier and therefore runs one tier per invocation."
        ),
    )
    ap.add_argument(
        "--util",
        type=float,
        default=UTIL_DEFAULT,
        help=(
            "offered load of the capped tier as a fraction of the link's "
            "capacity (default %(default)s).  Below 1.0 the sender never "
            "out-runs the link, so the reading is the link's delay instead "
            "of a queue the sender built up itself."
        ),
    )
    ap.add_argument(
        "--out",
        default=None,
        help="output JSON path (default build/e2e/bench_weaknet.json)",
    )
    args = ap.parse_args()

    tiers = TIERS
    if args.tier:
        wanted = set(args.tier)
        tiers = tuple(t for t in TIERS if t["id"] in wanted)

    restore_timer = _high_res_timer()

    try:
        if args.mode == "netem":
            results = [
                run_netem_tier(t, args.count, args.size, util=args.util)
                for t in tiers
            ]
        else:
            results = [
                run_tier(t, args.count, args.size, args.seed, util=args.util)
                for t in tiers
            ]
    finally:
        restore_timer()
    if args.mode == "netem":
        note = ("kernel-side impairments via 'tc qdisc netem'; the tool paces "
               "sends and measures arrival-minus-send; the capped tier is "
               "paced at util x the cap in on-wire bytes (UDP+IPv4+Ethernet "
               "headers included) so the limiter's queue stays empty; "
               "transport-floor baseline, not engine glass-to-glass")
    else:
        note = ("in-process userspace shaper over localhost UDP; deterministic "
                "seeded impairments; concurrent RX drain so latency is "
                "arrival-minus-send per packet; the capped tier is paced at "
                "util x the cap in payload bytes and its delay includes the "
                "link's serialization time, so the reading is the link's "
                "delay rather than a self-inflicted queue; transport-floor "
                "baseline, not engine glass-to-glass; the Linux 'tc qdisc "
                "netem' equivalent needs root and is driven by "
                "tools/benchmark_weaknet_netem.sh")
    result = {
        "mode": args.mode,
        "count_per_tier": args.count,
        "payload_bytes": args.size,
        "seed": args.seed,
        "util": args.util,
        "results": results,
        "note": note,
    }
    E2E.mkdir(parents=True, exist_ok=True)
    out = Path(args.out) if args.out else E2E / "bench_weaknet.json"
    out.write_text(json.dumps(result, indent=2), encoding="utf-8")
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        for r in results:
            print(f"[{r['tier']}] rx={r['received']}/{r['sent']} "
                  f"p50={r['p50_us']:.0f}us p99={r['p99_us']:.0f}us")
        print(f"wrote {out}")
    # In netem mode the kernel drops packets, so "received == sent" is
    # the wrong gate: the delivery ratio is the measurement and a loss
    # tier is *expected* to lose.  The gate there is that the link
    # delivered something and that the tiers without loss delivered
    # everything.
    if args.mode == "netem":
        ok = all(
            r["received"] > 0
            and (r["loss_config"] > 0 or r["received"] == r["sent"])
            for r in results
        )
    else:
        ok = all(r["received"] == r["expected"] for r in results)
    return 0 if ok else 2


if __name__ == "__main__":
    raise SystemExit(main())
