#!/usr/bin/env python3
# Copyright (c) 2026 NimRTC contributors.
# SPDX-License-Identifier: MIT
"""The remote-desktop weak-network benchmark (plan item **J4** / **J6**).

## What this measures -- and what it does not

The remote-desktop product is judged on *feel*.  A throughput number says
nothing about the feel: a session that delivers 20 Mbps of video but
freezes for a second every ten seconds is unusable, and a session that
delivers 2 Mbps of crisp, low-latency video is perfectly good.

So this tool reports the **response curve**, not a throughput number:

  * the **ladder level** vs. time -- the bitrate ladder's reaction to the
    link (the plan's Track D behaviour: step down under congestion,
    recover when the link frees up);
  * the **delivered fps** vs. time -- what the viewer actually saw;
  * the **cumulative freeze count** vs. time -- how often the picture
    stood still, and for how long;
  * the **latency percentiles** vs. time -- the end-to-end latency
    (capture -> render) in each window;
  * the **input-to-photon** percentiles (J6) -- the latency from the
    operator's input event to the frame that reflects it being displayed.

## The model

The scenario is a **synthetic remote-desktop model**: a source screen at
30 fps, a bitrate ladder, a rate-limited link with loss and jitter, a
bounded transmit queue, a decoder and a display.  It is deterministic
(seeded RNG), it needs no network, and it runs in under a second -- which
is what makes it usable as a **regression** benchmark: the artifact is
committed, so a change that makes the ladder react more slowly, or the
freeze count go up, is visible in the diff.

The numbers are a **model's** numbers, not a measurement of a real
session, and the artifact says so in its `notes`.  What the model *is*
good for is the shape of the response: how the ladder tracks a
collapsing link, how the freezes accumulate, how the latency moves when
the picture is re-encoded at a lower rung.

The tool reuses two things from the repo:

  - `tools/benchmark_weaknet.py`'s **transport-floor tiers** (the v1.1
    Q4 loss / jitter / cap tiers), so the impairment levels the
    remote-desktop scenario uses are the same ones the transport
    baseline uses and the two artifacts can be read side by side;
  - `tools/benchmark_capture_encode.py`'s **machine description**, so the
    artifact carries the same machine fields as the J5 artifact.

## The ladder model (Track D)

The ladder is a **monotone bitrate ladder**: every rung costs more
bitrate than the one below it.  It starts at the rung the profile's
resolution floor allows and reacts to two signals:

  * **congestion** -- the transmit queue built up (the modelled delay
    from the frame's ready time to its actual transmission).  Above the
    congestion threshold the ladder steps **down**, one rung at a time.
  * **headroom** -- the delivered throughput exceeds the next rung's
    bitrate and the queue has stayed empty.  After a hysteresis period
    the ladder steps **up**, one rung at a time.

The resolution floor (720p per the plan's software-path rule) is never
crossed, so the ladder's lowest rung is a floor, not a quality choice.

## Usage

    # Run the scenario and write the committed artifact.
    python tools/benchmark_remote_desktop.py \\
        --out docs/benchmarks/remote_desktop_weaknet_windows_x86_64.json

    # Print the summary only, write nothing.
    python tools/benchmark_remote_desktop.py --dry-run

    # A different impairment shape.
    python tools/benchmark_remote_desktop.py --seed 7 --duration 90
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import math
import platform
import random
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# ---------------------------------------------------------------------------
# The scenario
# ---------------------------------------------------------------------------

# The source screen: 30 fps, 1080p.  The *ladder* decides what actually
# goes on the wire; the source is what the capture device produces.
SOURCE_FPS = 30
SOURCE_WIDTH = 1920
SOURCE_HEIGHT = 1080

# The encode / decode / display costs, in milliseconds.  They are the
# hardware-path figures from the plan's §B.6 budget table (encode 4 ms,
# decode 4 ms, present 1 ms).
ENCODE_MS = 4.0
DECODE_MS = 4.0
RENDER_MS = 1.0

# The MTU-sized packet the payload is split into.
PACKET_BYTES = 1200

# The transmit queue's bound, in milliseconds.  A frame that waits longer
# than this is dropped (the queue overflows) -- which is what a real
# policer does, and what makes the congestion signal appear.
MAX_QUEUE_MS = 500.0

# A gap between two rendered frames longer than this counts as a freeze.
FREEZE_THRESHOLD_MS = 150.0

# The operator's input rate: 4 events a second (the plan's coalescer
# releases at most one pointer event per display frame; 4 Hz is a
# representative "moving the mouse" cadence).
INPUT_INTERVAL_S = 0.25

# The control channel's share of the link's delay.  The control plane
# rides its own (reliable, ordered) stack, so it sees the propagation
# delay but very little of the media queue.
CONTROL_DELAY_FACTOR = 0.5

# The ladder's rung list.  **Monotone** in bitrate: every rung costs
# more than the one below it (the plan's Track D requirement).
#
# The 1280x720 rung is the plan's **resolution floor** for a
# software-only machine (§B.6's software-path rule): the ladder never
# crosses it.
LADDER = (
    {"level": 0, "width": 1280, "height": 720, "fps": 15,
     "bitrate_kbps": 700},
    {"level": 1, "width": 1280, "height": 720, "fps": 30,
     "bitrate_kbps": 1500},
    {"level": 2, "width": 1920, "height": 1080, "fps": 30,
     "bitrate_kbps": 3500},
    {"level": 3, "width": 1920, "height": 1080, "fps": 30,
     "bitrate_kbps": 6000},
    {"level": 4, "width": 1920, "height": 1080, "fps": 30,
     "bitrate_kbps": 9000},
)

RESOLUTION_FLOOR = (1280, 720)

# The link's shape over the run.  Each phase is a stretch of time with a
# constant capacity, loss rate and jitter.
PHASES = (
    {"id": "clean", "duration_s": 15.0, "capacity_mbps": 20.0,
     "loss": 0.0, "jitter_ms": 4.0, "base_delay_ms": 10.0},
    {"id": "congested", "duration_s": 15.0, "capacity_mbps": 3.0,
     "loss": 0.02, "jitter_ms": 40.0, "base_delay_ms": 30.0},
    {"id": "collapse", "duration_s": 15.0, "capacity_mbps": 1.0,
     "loss": 0.08, "jitter_ms": 120.0, "base_delay_ms": 80.0},
    {"id": "recovery", "duration_s": 15.0, "capacity_mbps": 20.0,
     "loss": 0.0, "jitter_ms": 4.0, "base_delay_ms": 10.0},
)

# The controller's thresholds.
CONGESTION_QUEUE_MS = 50.0     # the queue depth that means "congested"
HEADROOM_QUEUE_MS = 10.0       # the queue depth that means "the link is free"
DOWNSTREAM_HOLD_S = 1.0        # how long the congestion must persist
UPSTREAM_HOLD_S = 3.0          # the hysteresis before a step up
UPSTREAM_HEADROOM = 1.20       # the delivered rate vs. the next rung


# ---------------------------------------------------------------------------
# Reusing the repo's other benchmark tools
# ---------------------------------------------------------------------------

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


def weaknet_tiers() -> list:
    """The transport-floor tiers from `tools/benchmark_weaknet.py`.

    They are recorded in the artifact so the two weak-network artifacts
    can be read side by side: the remote-desktop scenario's impairment
    levels are the same family as the transport baseline's.
    """
    mod = _load_module(ROOT / "tools" / "benchmark_weaknet.py",
                       "_nimrtc_benchmark_weaknet")
    if mod is None:
        return []
    return [dict(t) for t in getattr(mod, "TIERS", ())]


def describe_machine() -> dict:
    """The machine description, reusing the J5 tool's collector."""
    mod = _load_module(ROOT / "tools" / "benchmark_capture_encode.py",
                       "_nimrtc_benchmark_capture_encode")
    if mod is not None and hasattr(mod, "describe_machine"):
        try:
            return mod.describe_machine()
        except Exception:  # pragma: no cover
            pass
    # The fallback keeps the artifact's shape even if the sibling tool
    # cannot be loaded.
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
# Small numeric helpers
# ---------------------------------------------------------------------------

def _pct(values: list, q: float) -> float:
    """The nearest-rank percentile of an unsorted list (0 when empty)."""
    if not values:
        return 0.0
    ordered = sorted(values)
    idx = int(math.ceil(q * len(ordered))) - 1
    if idx < 0:
        idx = 0
    if idx >= len(ordered):
        idx = len(ordered) - 1
    return float(ordered[idx])


def _phase_at(t_s: float) -> dict:
    """The link's phase at `t_s`."""
    elapsed = 0.0
    for phase in PHASES:
        elapsed += phase["duration_s"]
        if t_s < elapsed:
            return phase
    return PHASES[-1]


def _phase_of(t_s: float) -> str:
    """The phase *name* at `t_s` (for the curve)."""
    return _phase_at(t_s)["id"]


# ---------------------------------------------------------------------------
# The ladder controller
# ---------------------------------------------------------------------------

class LadderController:
    """The plan's Track D ladder: monotone, steps down under congestion.

    The controller is driven by two observations the real one would
    have: the **queue depth** (how long a frame waited before the link
    took it) and the **delivered throughput** (how much actually got
    through in the recent window).
    """

    def __init__(self) -> None:
        self.level = 2  # the rung the session starts on
        self.changes: list[dict] = []
        self.bad_since = None
        self.good_since = None

    @property
    def rung(self) -> dict:
        return LADDER[self.level]

    def _can_step_down(self) -> bool:
        """True when the rung below is still at or above the floor."""
        if self.level == 0:
            return False
        below = LADDER[self.level - 1]
        return (below["width"], below["height"]) >= RESOLUTION_FLOOR

    def _step_down(self, t_s: float) -> None:
        if not self._can_step_down():
            return
        self.level -= 1
        self.changes.append({"t_s": round(t_s, 3), "direction": "down",
                             "level": self.level})
        self.good_since = None
        self.bad_since = None

    def _step_up(self, t_s: float) -> None:
        if self.level + 1 >= len(LADDER):
            return
        self.level += 1
        self.changes.append({"t_s": round(t_s, 3), "direction": "up",
                             "level": self.level})
        self.good_since = None
        self.bad_since = None

    def update(self, t_s: float, queue_ms: float, available_bps: float) -> None:
        """Feed one observation to the controller.

        @param queue_ms  how long the frame waited before the link took
                         it (the congestion signal).
        @param available_bps  the link's **estimated available**
                              bandwidth, in bits/s.  This is what a real
                              bandwidth estimator reports: the capacity
                              that is not already committed to the
                              backlog.  The delivered throughput would
                              be the wrong signal -- it is capped by the
                              rung the ladder is *on*, so the ladder
                              could never climb back out of a
                              congestion it caused itself.
        """
        congested = queue_ms > CONGESTION_QUEUE_MS

        if congested:
            self.good_since = None
            if self.bad_since is None:
                self.bad_since = t_s
            elif t_s - self.bad_since >= DOWNSTREAM_HOLD_S:
                self._step_down(t_s)
            return

        # The link looks free.  Stepping up needs *headroom*: the
        # available bandwidth has to exceed the next rung's bitrate.
        next_bps = LADDER[self.level + 1]["bitrate_kbps"] * 1000.0 \
            if self.level + 1 < len(LADDER) else 0.0
        headroom = (next_bps > 0.0
                    and queue_ms <= HEADROOM_QUEUE_MS
                    and available_bps >= next_bps * UPSTREAM_HEADROOM)
        if not headroom:
            self.good_since = None
            self.bad_since = None
            return

        if self.good_since is None:
            self.good_since = t_s
        elif t_s - self.good_since >= UPSTREAM_HOLD_S:
            self._step_up(t_s)


# ---------------------------------------------------------------------------
# The scenario
# ---------------------------------------------------------------------------

class ScenarioResult:
    """Everything the scenario produced."""

    def __init__(self) -> None:
        self.frames_sent = 0
        self.frames_delivered = 0
        self.frames_lost = 0
        self.queue_overflows = 0
        self.latencies_ms: list = []
        self.input_to_photon_ms: list = []
        self.freezes: list = []          # {"t_s", "duration_ms"}
        self.curve: list = []
        self.ladder_changes: list = []
        self.level_time_s: list = []      # (t_s, level)
        self.rendered: list = []          # (render_time_s, latency_ms)

    def latencies_in(self, start_s: float, end_s: float) -> list:
        """The end-to-end latencies of the frames rendered in a window."""
        return [lat for render_t, lat in self.rendered
                if start_s <= render_t <= end_s]

    def level_at(self, t_s: float) -> int:
        """The ladder level at `t_s`."""
        level = LADDER[0]["level"]
        for ts, lv in self.level_time_s:
            if ts > t_s:
                break
            level = lv
        return level


def run_scenario(seed: int, duration_s: float) -> ScenarioResult:
    """Run the synthetic remote-desktop scenario.

    The model is a source at `SOURCE_FPS`, a ladder, a rate-limited link
    with loss and jitter, a bounded transmit queue, a decoder and a
    display.  Everything is driven by the seeded RNG, so the run is
    reproducible.
    """
    rng = random.Random(seed)
    result = ScenarioResult()
    ladder = LadderController()

    total_frames = int(round(duration_s * SOURCE_FPS))

    # The link's state.
    link_free_at_s = 0.0
    # The delivered bytes, for the throughput signal.
    delivered: list = []               # (render_time_s, bytes)
    # The frame's capture time per frame index.
    frame_capture_times: list = []     # capture time per frame index
    frame_render_times: list = []      # render time per frame index (None
                                        # when the frame was lost)
    frame_bytes: list = []

    # ---- the source frames ------------------------------------------------
    for index in range(total_frames):
        capture_t = index / SOURCE_FPS
        frame_capture_times.append(capture_t)

        rung = ladder.rung
        # The frame's size at the rung's frame rate.
        bytes_per_frame = (rung["bitrate_kbps"] * 1000.0 / 8.0) / rung["fps"]
        frame_bytes.append(bytes_per_frame)

        # The rung's frame rate caps what the encoder emits; the source
        # produces more, and the extra frames are the encoder's to skip
        # (the plan's §B.6 frame-skip rule).
        if (index % max(1, int(round(SOURCE_FPS / rung["fps"])))) != 0:
            frame_render_times.append(None)
            continue

        result.frames_sent += 1

        phase = _phase_at(capture_t)
        capacity_bps = phase["capacity_mbps"] * 1e6

        # ---- the link -----------------------------------------------------
        ready_t = capture_t + ENCODE_MS / 1000.0
        start_t = max(ready_t, link_free_at_s)
        queue_ms = (start_t - ready_t) * 1000.0

        if queue_ms > MAX_QUEUE_MS:
            # The queue overflowed: the frame never went on the wire.
            result.queue_overflows += 1
            result.frames_lost += 1
            frame_render_times.append(None)
            _update_controller(result, ladder, capture_t, queue_ms,
                               capacity_bps)
            continue

        serialization_s = (bytes_per_frame * 8.0) / capacity_bps
        link_free_at_s = start_t + serialization_s

        # The packets: each is dropped independently.
        packets = max(1, int(math.ceil(bytes_per_frame / PACKET_BYTES)))
        lost = False
        max_delay_s = 0.0
        for _ in range(packets):
            if rng.random() < phase["loss"]:
                lost = True
                break
            delay_ms = (phase["base_delay_ms"]
                        + rng.random() * phase["jitter_ms"])
            max_delay_s = max(max_delay_s, delay_ms / 1000.0)

        if lost:
            result.frames_lost += 1
            frame_render_times.append(None)
        else:
            arrival_t = start_t + max_delay_s
            render_t = arrival_t + (DECODE_MS + RENDER_MS) / 1000.0
            frame_render_times.append(render_t)
            result.frames_delivered += 1
            latency_ms = (render_t - capture_t) * 1000.0
            result.latencies_ms.append(latency_ms)
            result.rendered.append((render_t, latency_ms))
            delivered.append((render_t, bytes_per_frame))

        # ---- the controller ------------------------------------------------
        _update_controller(result, ladder, capture_t, queue_ms, capacity_bps)

    result.ladder_changes = ladder.changes

    # ---- the freezes -------------------------------------------------------
    result.freezes = _detect_freezes(result.rendered)

    # ---- the input-to-photon pairing (J6) ---------------------------------
    result.input_to_photon_ms = _pair_inputs(
        frame_capture_times, frame_render_times,
        duration_s=duration_s, rng=rng)

    # ---- the response curve ------------------------------------------------
    result.curve = _build_curve(result, duration_s)

    return result


def _update_controller(result: "ScenarioResult", ladder: LadderController,
                       t_s: float, queue_ms: float, capacity_bps: float) -> None:
    """Feed the ladder controller one observation.

    The available-bandwidth estimate is the link's capacity minus what
    the backlog already holds.  A real bandwidth estimator reports the
    same thing (the capacity that is not already committed to the
    queue); the delivered throughput would be the wrong signal, because
    it is capped by the rung the ladder is *on* -- so the ladder could
    never climb back out of a congestion it caused itself.
    """
    queue_fraction = min(1.0, queue_ms / MAX_QUEUE_MS)
    available_bps = capacity_bps * (1.0 - queue_fraction)
    ladder.update(t_s, queue_ms, available_bps)
    result.level_time_s.append((round(t_s, 3), ladder.level))


def _delivered_bps(delivered: list, now_s: float, window_s: float = 2.0) -> float:
    """The delivered throughput over the trailing window, in bits/s."""
    cutoff = now_s - window_s
    total = 0.0
    for t_s, nbytes in delivered:
        if t_s < cutoff:
            continue
        total += nbytes * 8.0
    span = min(window_s, max(now_s - cutoff, 1e-6))
    return total / span if span > 0 else 0.0


def _detect_freezes(rendered: list) -> list:
    """The freezes: a gap between two rendered frames beyond the threshold."""
    freezes = []
    for i in range(1, len(rendered)):
        gap_ms = (rendered[i][0] - rendered[i - 1][0]) * 1000.0
        if gap_ms > FREEZE_THRESHOLD_MS:
            freezes.append({
                "t_s": round(rendered[i][0], 3),
                "duration_ms": round(gap_ms, 3),
            })
    return freezes


def _pair_inputs(capture_times: list, render_times: list,
                 duration_s: float, rng: random.Random) -> list:
    """Pair the operator's input events with the frames that reflect them.

    An input event sent at `t` reaches the remote machine after the
    control channel's delay; the **first source frame captured after
    that** is the frame that reflects the event.  Its display time is
    the photon.  The input-to-photon latency is the difference.
    """
    out = []
    t = 0.0
    while t < duration_s:
        phase = _phase_at(t)
        control_delay_s = ((phase["base_delay_ms"] * CONTROL_DELAY_FACTOR)
                           + rng.random() * phase["jitter_ms"] * 0.25) / 1000.0
        arrive_at = t + control_delay_s

        # The first frame captured at or after the event's arrival.
        index = None
        for i, capture_t in enumerate(capture_times):
            if capture_t >= arrive_at:
                index = i
                break
        if index is None or render_times[index] is None:
            # The frame that would have reflected the event was lost:
            # there is no photon to measure.
            t += INPUT_INTERVAL_S
            continue

        out.append((render_times[index] - t) * 1000.0)
        t += INPUT_INTERVAL_S
    return out


def _build_curve(result: ScenarioResult, duration_s: float) -> list:
    """The response curve: one sample every 0.5 s."""
    curve = []
    step_s = 0.5
    t = 0.0
    while t < duration_s:
        # The delivered frames inside the trailing second.
        window_start = t - 1.0
        window = [lat for render_t, lat in result.rendered
                  if window_start <= render_t <= t]
        delivered_fps = float(len(window))

        latencies = result.latencies_in(window_start, t)

        # The ladder level at `t`.
        level = result.level_at(t)

        # The cumulative freeze count up to `t`.
        freezes = sum(1 for f in result.freezes if f["t_s"] <= t)

        curve.append({
            "t_s": round(t, 3),
            "phase": _phase_of(t),
            "ladder_level": level,
            "ladder_bitrate_kbps": LADDER[level]["bitrate_kbps"],
            "ladder_resolution": f"{LADDER[level]['width']}x{LADDER[level]['height']}",
            "delivered_fps": round(delivered_fps, 3),
            "freeze_count": freezes,
            "latency_p50_ms": round(_pct(latencies, 0.50), 3),
            "latency_p95_ms": round(_pct(latencies, 0.95), 3),
        })
        t += step_s
    return curve


# ---------------------------------------------------------------------------
# The summary
# ---------------------------------------------------------------------------

def build_summary(result: ScenarioResult, duration_s: float) -> dict:
    """The headline numbers the artifact carries."""
    lat = result.latencies_ms
    i2p = result.input_to_photon_ms

    freeze_total_ms = sum(f["duration_ms"] for f in result.freezes)

    by_phase: dict = {}
    for entry in result.curve:
        phase = entry["phase"]
        slot = by_phase.setdefault(phase, {
            "phase": phase,
            "samples": 0,
            "delivered_fps": [],
            "ladder_levels": [],
            "latency_p50_ms": [],
            "freeze_count": [],
        })
        slot["samples"] += 1
        slot["delivered_fps"].append(entry["delivered_fps"])
        slot["ladder_levels"].append(entry["ladder_level"])
        slot["latency_p50_ms"].append(entry["latency_p50_ms"])
        slot["freeze_count"].append(entry["freeze_count"])

    phase_summary = []
    for phase in PHASES:
        slot = by_phase.get(phase["id"])
        if not slot:
            continue
        phase_summary.append({
            "phase": phase["id"],
            "capacity_mbps": phase["capacity_mbps"],
            "loss": phase["loss"],
            "jitter_ms": phase["jitter_ms"],
            "delivered_fps_mean": round(sum(slot["delivered_fps"])
                                        / len(slot["delivered_fps"]), 3),
            "ladder_level_min": min(slot["ladder_levels"]),
            "ladder_level_max": max(slot["ladder_levels"]),
            "latency_p50_ms_mean": round(sum(slot["latency_p50_ms"])
                                        / len(slot["latency_p50_ms"]), 3),
            "freeze_count_final": slot["freeze_count"][-1],
        })

    return {
        "duration_s": duration_s,
        "frames_sent": result.frames_sent,
        "frames_delivered": result.frames_delivered,
        "frames_lost": result.frames_lost,
        "queue_overflows": result.queue_overflows,
        "delivery_ratio": (result.frames_delivered / result.frames_sent
                           if result.frames_sent else 0.0),
        "freeze_count": len(result.freezes),
        "freeze_total_ms": round(freeze_total_ms, 3),
        "freeze_longest_ms": round(max((f["duration_ms"] for f in result.freezes),
                                       default=0.0), 3),
        "freeze_ratio": (freeze_total_ms / (duration_s * 1000.0)
                         if duration_s > 0 else 0.0),
        "latency_ms": {
            "p50": round(_pct(lat, 0.50), 3),
            "p95": round(_pct(lat, 0.95), 3),
            "p99": round(_pct(lat, 0.99), 3),
            "max": round(max(lat) if lat else 0.0, 3),
        },
        "input_to_photon_ms": {
            "p50": round(_pct(i2p, 0.50), 3),
            "p95": round(_pct(i2p, 0.95), 3),
            "max": round(max(i2p) if i2p else 0.0, 3),
            "samples": len(i2p),
        },
        "ladder_changes": result.ladder_changes,
        "ladder_step_downs": sum(1 for c in result.ladder_changes
                                 if c["direction"] == "down"),
        "ladder_step_ups": sum(1 for c in result.ladder_changes
                               if c["direction"] == "up"),
        "by_phase": phase_summary,
    }


def print_summary(doc: dict) -> None:
    s = doc["summary"]
    machine = doc.get("machine", {})
    print(f"machine : {machine.get('cpu_model') or 'unknown CPU'} "
          f"({machine.get('os')} {machine.get('machine')})")
    print()
    print("remote-desktop response curve (the summary of the curve)")
    print(f"  {'phase':>10} {'cap Mbps':>9} {'fps':>7} {'ladder':>10} "
          f"{'p50 ms':>8} {'freezes':>8}")
    for p in s["by_phase"]:
        print(f"  {p['phase']:>10} {p['capacity_mbps']:>9.1f} "
              f"{p['delivered_fps_mean']:>7.1f} "
              f"{p['ladder_level_min']}-{p['ladder_level_max']:<7} "
              f"{p['latency_p50_ms_mean']:>8.1f} {p['freeze_count_final']:>8}")
    print()
    print(f"frames       : {s['frames_delivered']}/{s['frames_sent']} delivered "
          f"({s['delivery_ratio'] * 100:.1f} %), "
          f"{s['queue_overflows']} queue overflows")
    print(f"freezes      : {s['freeze_count']} "
          f"({s['freeze_total_ms']:.0f} ms total, "
          f"{s['freeze_longest_ms']:.0f} ms longest, "
          f"ratio {s['freeze_ratio'] * 100:.2f} %)")
    print(f"latency (ms) : p50 {s['latency_ms']['p50']:.1f}  "
          f"p95 {s['latency_ms']['p95']:.1f}  "
          f"p99 {s['latency_ms']['p99']:.1f}  "
          f"max {s['latency_ms']['max']:.1f}")
    print(f"input->photon: p50 {s['input_to_photon_ms']['p50']:.1f} ms  "
          f"p95 {s['input_to_photon_ms']['p95']:.1f} ms  "
          f"({s['input_to_photon_ms']['samples']} samples)")
    print(f"ladder       : {s['ladder_step_downs']} down / "
          f"{s['ladder_step_ups']} up")
    print()
    print("the curve (every 2 s):")
    print(f"  {'t s':>6} {'phase':>10} {'level':>6} {'fps':>7} "
          f"{'freezes':>8} {'p50 ms':>8}")
    for entry in doc["curve"]:
        if int(entry["t_s"] * 2) % 4 != 0:
            continue
        print(f"  {entry['t_s']:>6.1f} {entry['phase']:>10} "
              f"{entry['ladder_level']:>6} {entry['delivered_fps']:>7.1f} "
              f"{entry['freeze_count']:>8} {entry['latency_p50_ms']:>8.1f}")


# ---------------------------------------------------------------------------
# The artifact
# ---------------------------------------------------------------------------

def build_document(seed: int, duration_s: float, result: ScenarioResult) -> dict:
    return {
        "schema": 1,
        "benchmark": "remote_desktop_weaknet",
        "generated_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "plan": "docs/plan/remote-desktop-plan.md",
        "plan_item": "J4",
        "plan_item_secondary": "J6",
        "seed": seed,
        "deterministic": True,
        "machine": describe_machine(),
        "scenario": {
            "source_fps": SOURCE_FPS,
            "source_resolution": f"{SOURCE_WIDTH}x{SOURCE_HEIGHT}",
            "duration_s": duration_s,
            "encode_ms": ENCODE_MS,
            "decode_ms": DECODE_MS,
            "render_ms": RENDER_MS,
            "packet_bytes": PACKET_BYTES,
            "max_queue_ms": MAX_QUEUE_MS,
            "freeze_threshold_ms": FREEZE_THRESHOLD_MS,
            "input_interval_s": INPUT_INTERVAL_S,
            "control_delay_factor": CONTROL_DELAY_FACTOR,
            "ladder": [dict(rung) for rung in LADDER],
            "resolution_floor": f"{RESOLUTION_FLOOR[0]}x{RESOLUTION_FLOOR[1]}",
            "phases": [dict(p) for p in PHASES],
            "controller": {
                "congestion_queue_ms": CONGESTION_QUEUE_MS,
                "headroom_queue_ms": HEADROOM_QUEUE_MS,
                "downstream_hold_s": DOWNSTREAM_HOLD_S,
                "upstream_hold_s": UPSTREAM_HOLD_S,
                "upstream_headroom": UPSTREAM_HEADROOM,
            },
        },
        "transport_floor_reference": {
            "source": "tools/benchmark_weaknet.py",
            "tiers": weaknet_tiers(),
        },
        "summary": build_summary(result, duration_s),
        "curve": result.curve,
        "freezes": result.freezes,
        "notes": [
            "These are the numbers of a SYNTHETIC remote-desktop model, "
            "not of a real session: a source screen, a bitrate ladder, a "
            "rate-limited link with loss and jitter, a bounded transmit "
            "queue, a decoder and a display. The run is deterministic "
            "(seeded RNG), so the artifact is a regression baseline.",
            "The response curve -- ladder level, delivered fps, cumulative "
            "freeze count and latency percentiles vs. time -- is the "
            "measurement the plan asks for (J4). A throughput number is "
            "not reported: throughput does not describe the feel.",
            "The input-to-photon numbers (J6) pair the operator's input "
            "event with the first source frame captured after the event "
            "reaches the remote machine, and measure the event to that "
            "frame's display time.",
            "The stage costs (encode 4 ms, decode 4 ms, present 1 ms) are "
            "the hardware-path figures from the plan's §B.6 budget table.",
            "The impairment levels are the same family as the "
            "transport-floor tiers of tools/benchmark_weaknet.py; the two "
            "artifacts can be read side by side.",
        ],
    }


def check_invariants(doc: dict) -> list:
    """The scenario's own sanity checks.

    These are checks of the *model*, not of a product: a scenario that
    does not react to a collapsing link would make the artifact
    meaningless.
    """
    problems = []
    s = doc["summary"]

    if s["frames_delivered"] <= 0:
        problems.append("the scenario delivered no frame at all")
    if s["delivery_ratio"] <= 0.0:
        problems.append("the delivery ratio is zero")

    # The ladder must react to the collapse: the link drops to 1 Mbps,
    # which cannot carry the top rungs.
    if s["ladder_step_downs"] == 0:
        problems.append(
            "the ladder never stepped down under the 1 Mbps collapse -- "
            "the response curve would be meaningless")

    # The ladder must not cross the resolution floor.
    floor_w, floor_h = RESOLUTION_FLOOR
    for rung in LADDER:
        if (rung["width"], rung["height"]) < (floor_w, floor_h):
            problems.append(
                f"ladder rung {rung['level']} "
                f"({rung['width']}x{rung['height']}) is below the "
                f"resolution floor")

    # The collapse phase must actually be worse than the clean phase.
    by_phase = {p["phase"]: p for p in s["by_phase"]}
    clean = by_phase.get("clean")
    collapse = by_phase.get("collapse")
    if clean and collapse:
        if collapse["delivered_fps_mean"] > clean["delivered_fps_mean"]:
            problems.append(
                "the collapse phase delivered MORE frames than the clean "
                "phase -- the impairment is not reaching the model")
        if collapse["ladder_level_min"] >= clean["ladder_level_max"]:
            problems.append(
                "the ladder did not step down during the collapse phase")

    # The input-to-photon measurement must exist.
    if s["input_to_photon_ms"]["samples"] == 0:
        problems.append("no input-to-photon sample was produced")

    # The curve must cover the whole run.
    if not doc["curve"]:
        problems.append("the response curve is empty")
    else:
        span = doc["curve"][-1]["t_s"]
        if span < doc["summary"]["duration_s"] - 1.0:
            problems.append(
                f"the response curve covers only {span:.1f} s of the "
                f"{doc['summary']['duration_s']:.1f} s run")

    return problems


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seed", type=int, default=20261114,
                    help="the RNG seed; the run is deterministic (default: "
                         "%(default)s)")
    ap.add_argument("--duration", type=float, default=60.0,
                    help="the scenario length in seconds (default: "
                         "%(default)s)")
    ap.add_argument("--out", type=Path, default=None,
                    help="where to write the artifact (default: "
                         "docs/benchmarks/remote_desktop_weaknet_"
                         "<os>_<arch>.json)")
    ap.add_argument("--dry-run", action="store_true",
                    help="run the scenario and print the summary, but write "
                         "no artifact")
    args = ap.parse_args()

    if args.duration <= 0:
        raise SystemExit("error: --duration must be positive")

    result = run_scenario(args.seed, args.duration)
    doc = build_document(args.seed, args.duration, result)

    problems = check_invariants(doc)
    if problems:
        for p in problems:
            sys.stderr.write(f"error: {p}\n")
        return 1

    if not args.dry_run:
        if args.out is not None:
            out = args.out
        else:
            os_name = platform.system().lower()
            arch = platform.machine().lower()
            if arch == "amd64":
                arch = "x86_64"
            out = (ROOT / "docs" / "benchmarks"
                   / f"remote_desktop_weaknet_{os_name}_{arch}.json")
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
        print(f"wrote {out}")
    else:
        print("(dry run: no artifact written)")

    print_summary(doc)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
