#!/usr/bin/env python3
"""
signaling_proxy.py — Bridges demo-p2p's stdin/stdout to a WebSocket signaling
server.  Used by the Chrome interop harness to talk to NimRTC.

Usage:
    python signaling_proxy.py \
        --signaling ws://localhost:8765/interop \
        --binary build/examples/demo-p2p/demo-p2p.exe \
        [--answerer] [--duration 60]

Each line read from demo-p2p's stdout (a JSON object with at least "type") is
forwarded verbatim to the signaling server.  Each line read from the WebSocket
is written verbatim to demo-p2p's stdin.

Why a separate proxy?
    demo-p2p is a single-process C++ binary that wants to be cross-platform
    and dependency-free.  Adding a WebSocket client + SHA-1 + base64 to it
    for a single use-case (interop) is the wrong place.  The proxy keeps
    the heavy lifting in Python (where `websockets` is already available)
    and lets demo-p2p stay focused on the engine.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import os
import shlex
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Optional

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)-8s] %(name)s: %(message)s",
)
logger = logging.getLogger("signaling_proxy")

try:
    import websockets
except ImportError:
    websockets = None
    logger.warning("websockets package missing — install with: pip install websockets")


# -----------------------------------------------------------------------------


class NimRTCSubprocess:
    """Spawn demo-p2p in --signaling-proxy mode, wire its I/O to the proxy."""

    def __init__(self, binary: Path, answerer: bool, duration: int,
                 bind: str = "", stun_host: str = "", stun_port: int = 0,
                 no_stun: bool = False):
        self.binary = binary
        self.answerer = answerer
        self.duration = duration
        self.bind = bind
        self.stun_host = stun_host
        self.stun_port = stun_port
        self.no_stun = no_stun
        self.proc: Optional[subprocess.Popen] = None
        self._queue: asyncio.Queue[str] = asyncio.Queue()
        self._closed = False
        self._stop_reason: Optional[str] = None

    async def start(self) -> None:
        if not self.binary.exists():
            raise FileNotFoundError(self.binary)
        cmd = [str(self.binary), "--signaling-proxy",
               "--duration", str(self.duration)]
        if self.answerer:
            cmd.append("--answerer")
        if self.bind:
            cmd += ["--bind", self.bind]
        if self.no_stun:
            cmd.append("--no-stun")
        elif self.stun_host:
            cmd += ["--stun-host", self.stun_host]
            if self.stun_port:
                cmd += ["--stun-port", str(self.stun_port)]
        logger.info("Spawning demo-p2p: %s", shlex.join(cmd))
        self.proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=sys.stdout,       # merge engine stderr into our stdout
            bufsize=0,                 # unbuffered for line-by-line read
        )
        # Reader thread for stdout → asyncio queue.
        t = threading.Thread(target=self._read_stdout, daemon=True)
        t.start()

    def _read_stdout(self) -> None:
        assert self.proc and self.proc.stdout
        for raw in self.proc.stdout:
            if self._closed:
                break
            line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            if not line:
                continue
            try:
                self._queue.put_nowait(line)
            except Exception as e:
                logger.warning("queue put failed: %s", e)

    async def next_message(self, timeout: float = 30.0) -> Optional[str]:
        """Return the next JSON line emitted by demo-p2p, or None on EOF."""
        try:
            return await asyncio.wait_for(self._queue.get(), timeout)
        except asyncio.TimeoutError:
            return None

    async def send(self, msg: str) -> None:
        if not self.proc or not self.proc.stdin:
            return
        try:
            self.proc.stdin.write((msg + "\n").encode("utf-8"))
            await asyncio.to_thread(self.proc.stdin.flush)
        except BrokenPipeError:
            logger.warning("demo-p2p stdin closed")

    async def wait_exit(self) -> int:
        if not self.proc:
            return -1
        # Wait indefinitely; demo-p2p is responsible for exiting when its
        # internal duration expires.  We don't impose our own timeout here.
        return await asyncio.to_thread(self.proc.wait)

    def stop(self) -> None:
        if not self.proc:
            return
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(2)
            except subprocess.TimeoutExpired:
                self.proc.kill()


# -----------------------------------------------------------------------------


async def pump_subprocess_to_ws(sub: NimRTCSubprocess, ws) -> None:
    """Forward JSON lines from demo-p2p to the WebSocket."""
    while not sub._closed:
        msg = await sub.next_message(timeout=1.0)
        if msg is None:
            if sub.proc and sub.proc.poll() is not None:
                logger.info("demo-p2p exited with %s", sub.proc.returncode)
                sub._closed = True
                break
            continue
        # Validate that the line parses as JSON before sending.
        try:
            parsed = json.loads(msg)
        except json.JSONDecodeError:
            logger.warning("demo-p2p emitted non-JSON: %s", msg[:120])
            continue
        mtype = parsed.get("type", "?")
        sender = parsed.get("from", "")
        if sender:
            logger.info("demo-p2p >> %s (from %s)", mtype, sender)
        else:
            logger.info("demo-p2p >> %s", mtype)
        await ws.send(msg)


async def pump_ws_to_subprocess(ws, sub: NimRTCSubprocess) -> None:
    """Forward JSON messages from the WebSocket to demo-p2p's stdin."""
    try:
        async for raw in ws:
            msg = raw.decode("utf-8") if isinstance(raw, bytes) else raw
            try:
                parsed = json.loads(msg)
            except json.JSONDecodeError:
                logger.warning("WS sent non-JSON: %s", msg[:120])
                continue
            mtype = parsed.get("type", "?")
            sender = parsed.get("from", "")
            if sender:
                logger.info("WS >> %s (from %s)", mtype, sender)
            else:
                logger.info("WS >> %s", mtype)
            await sub.send(msg)
            if mtype == "bye":
                logger.info("received bye, stopping")
                sub._closed = True
                break
    except websockets.exceptions.ConnectionClosed:
        logger.info("WebSocket closed")
        sub._closed = True


# -----------------------------------------------------------------------------


async def main() -> int:
    if websockets is None:
        logger.error("websockets package not installed — pip install websockets")
        return 1

    ap = argparse.ArgumentParser()
    ap.add_argument("--signaling", "-s",
                    default=os.environ.get("NIMRTC_SIGNALING_WS",
                                           "ws://localhost:8765/interop"))
    ap.add_argument("--binary", "-b",
                    default=os.environ.get("NIMRTC_BINARY",
                                           "build/examples/demo-p2p/demo-p2p.exe"))
    ap.add_argument("--answerer", action="store_true",
                    help="NimRTC acts as the answerer (Chrome is offerer)")
    ap.add_argument("--duration", type=int, default=60,
                    help="Max session duration in seconds")
    ap.add_argument("--bind", default="",
                    help="Local ICE bind address (e.g. 0.0.0.0)")
    ap.add_argument("--stun-host", default="",
                    help="STUN server hostname override")
    ap.add_argument("--stun-port", type=int, default=0,
                    help="STUN server port override")
    ap.add_argument("--no-stun", action="store_true",
                    help="Disable STUN candidate gathering")
    args = ap.parse_args()

    binary = Path(args.binary)
    if not binary.exists():
        # Try a few common locations.
        candidates = [
            Path("build/examples/demo-p2p/demo-p2p.exe"),
            Path("build/examples/demo-p2p/demo-p2p"),
            Path("../build/examples/demo-p2p/demo-p2p.exe"),
        ]
        for c in candidates:
            if c.exists():
                binary = c.resolve()
                break
        else:
            logger.error("demo-p2p binary not found at %s", binary)
            return 2

    sub = NimRTCSubprocess(binary, args.answerer, args.duration,
                           bind=args.bind, stun_host=args.stun_host,
                           stun_port=args.stun_port, no_stun=args.no_stun)
    await sub.start()

    logger.info("Connecting to signaling server: %s", args.signaling)
    try:
        async with websockets.connect(args.signaling) as ws:
            logger.info("WebSocket connected — bridging")
            send_task = asyncio.create_task(pump_ws_to_subprocess(ws, sub))
            recv_task = asyncio.create_task(pump_subprocess_to_ws(sub, ws))
            done, pending = await asyncio.wait(
                {send_task, recv_task, asyncio.create_task(sub.wait_exit())},
                return_when=asyncio.FIRST_COMPLETED,
            )
            for t in pending:
                t.cancel()
    except websockets.exceptions.WebSocketException as e:
        logger.error("WebSocket error: %s", e)
        sub.stop()
        return 3

    sub.stop()
    rc = sub.proc.returncode if sub.proc else 0
    logger.info("signaling_proxy exiting (demo-p2p rc=%s)", rc)
    return rc if rc is not None else 0


if __name__ == "__main__":
    try:
        sys.exit(asyncio.run(main()))
    except KeyboardInterrupt:
        logger.info("interrupted")
