#!/usr/bin/env python3
"""Pair `rd_viewer` and `rd_device` over a local signalling relay.

The two examples speak newline-delimited JSON on stdin/stdout (the
`rd_signalling.hpp` protocol).  This script is the missing half of that
protocol: it runs both processes and shuttles their lines between them,
so a full remote-desktop session can be exercised on one machine without
a signalling server.

    python tools/rd_pair.py [--duration SECONDS] [--build-dir DIR]

The device runs with `--no-inject`: the point of the harness is the
*media* path (capture -> encode -> transport -> decode -> render) plus
the *control* path (input -> filter -> inject), and injecting real mouse
and keyboard events into the developer's own desktop is not something a
test harness should do by surprise.  Drop the flag to exercise the
injector as well.
"""

import argparse
import json
import subprocess
import sys
import threading
from pathlib import Path
from typing import Optional


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--duration", type=int, default=8,
        help="how long each side should run, in seconds")
    parser.add_argument(
        "--build-dir", type=Path, default=None,
        help="the directory holding rd_viewer.exe / rd_device.exe")
    parser.add_argument(
        "--inject", action="store_true",
        help="let the device inject the received input into the desktop")
    parser.add_argument(
        "--verbose", action="store_true",
        help="print every signalling line")
    return parser.parse_args()


def find_exe_dir(explicit: Optional[Path]) -> Path:
    """Locate the directory that holds the two example binaries."""
    if explicit is not None:
        return explicit

    root = Path(__file__).resolve().parent.parent
    candidates = [
        root / "build" / "examples" / "Debug",
        root / "build" / "examples" / "Release",
        root / "build" / "b11-on" / "examples" / "Debug",
        root / "build" / "ci-msvc" / "examples" / "Debug",
    ]
    for candidate in candidates:
        if (candidate / "rd_viewer.exe").exists():
            return candidate
    # Fall back to a search so a fresh build directory still works.
    for viewer in (root / "build").rglob("rd_viewer.exe"):
        return viewer.parent
    raise SystemExit("rd_viewer.exe was not found -- build the examples first")


def main() -> int:
    args = parse_args()
    exe_dir = find_exe_dir(args.build_dir)

    viewer_cmd = [str(exe_dir / "rd_viewer.exe"), "--no-stun",
                  "--duration", str(args.duration)]
    device_cmd = [str(exe_dir / "rd_device.exe"), "--no-stun",
                  "--duration", str(args.duration)]
    if not args.inject:
        device_cmd.append("--no-inject")

    print(f"viewer: {viewer_cmd}", flush=True)
    print(f"device: {device_cmd}", flush=True)

    viewer = subprocess.Popen(viewer_cmd, stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=None)
    device = subprocess.Popen(device_cmd, stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=None)

    def forward(src, dst, tag: str) -> None:
        for raw in src:
            line = raw.decode("utf-8", "replace").strip()
            if not line:
                continue
            if args.verbose:
                print(f"[{tag}] {line[:200]}", flush=True)
            try:
                msg = json.loads(line)
            except json.JSONDecodeError as exc:
                print(f"[{tag}] BAD JSON: {exc}", flush=True)
                continue
            dst.write((json.dumps(msg) + "\n").encode())
            dst.flush()

    threading.Thread(target=forward, args=(viewer.stdout, device.stdin, "v->d"),
                     daemon=True).start()
    threading.Thread(target=forward, args=(device.stdout, viewer.stdin, "d->v"),
                     daemon=True).start()

    viewer.wait(timeout=args.duration + 20)
    device.wait(timeout=10)
    print(f"rc viewer={viewer.returncode} device={device.returncode}", flush=True)
    return 0 if viewer.returncode == 0 and device.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
