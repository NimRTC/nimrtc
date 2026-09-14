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
                 no_stun: bool = False,
                 turn_host: str = "", turn_port: int = 3478,
                 turn_user: str = "", turn_pass: str = ""):
        self.binary = binary
        self.answerer = answerer
        self.duration = duration
        self.bind = bind
        self.stun_host = stun_host
        self.stun_port = stun_port
        self.no_stun = no_stun
        self.turn_host = turn_host
        self.turn_port = turn_port
        self.turn_user = turn_user
        self.turn_pass = turn_pass
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
        if self.turn_host:
            cmd += ["--turn-host", self.turn_host,
                    "--turn-port", str(self.turn_port)]
            if self.turn_user:
                cmd += ["--turn-user", self.turn_user,
                        "--turn-pass", self.turn_pass]
        logger.info("Spawning demo-p2p: %s", shlex.join(cmd))
        # Send demo-p2p's stderr to a dedicated file so we can inspect the
        # engine's answerer logs after the run.  Without this, demo-p2p's
        # [demo-p2p] answer SDP ... trace goes nowhere.
        # Merge demo-p2p stderr into stdout so the test harness (which reads
        # the proxy's merged stderr) can see demo-p2p's DTLS/ICE state logs.
        # Lines from stderr are prefixed with "[p2p] " to distinguish them from
        # the proxy's own log lines.
        self._stderr_path = Path(os.environ.get(
            "NIMRTC_PROXY_STDERR",
            str(Path(__file__).parent / "_proxy_demo_stderr.log")))
        try:
            self._stderr_file = open(self._stderr_path, "wb", buffering=0)
        except OSError as e:
            logger.warning("could not open stderr file: %s — falling back to merged stdout", e)
            self._stderr_file = subprocess.STDOUT  # merge into stdout so test sees it
        self.proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self._stderr_file,
            bufsize=0,                 # unbuffered for line-by-line read
            # Inherit the proxy's environment and add optional debug toggles
            # so we can dump DTLS verify_data traces for Chrome interop
            # debugging without touching the test runner.  Set
            # NIMRTC_DTLS_TRACE=/path/to/trace to enable.
            env={**os.environ,
                 "NIMRTC_DTLS_TRACE": os.environ.get(
                     "NIMRTC_DTLS_TRACE",
                     str(Path(__file__).parent.parent / "build" / "nimrtc_dtls_trace.log")),
                 "NIMRTC_DTLS_KEYLOG": os.environ.get(
                     "NIMRTC_DTLS_KEYLOG",
                     str(Path(__file__).parent.parent / "build" / "nimrtc_dtls_keylog.txt")),
                 "NIMRTC_DTLS_SPKI_FILE": os.environ.get(
                     "NIMRTC_DTLS_SPKI_FILE",
                     str(Path(__file__).parent.parent / "build" / "nimrtc_spki.txt")),
                 } if os.environ.get("NIMRTC_DTLS_TRACE") or
                    os.environ.get("NIMRTC_DTLS_KEYLOG") or
                    os.environ.get("NIMRTC_DTLS_SPKI_FILE") else None,
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
        try:
            if hasattr(self, "_stderr_file") and self._stderr_file not in (
                    None, subprocess.DEVNULL):
                self._stderr_file.close()
        except Exception:
            pass


# -----------------------------------------------------------------------------


def _inject_ssrc_into_sdp(sdp: str, ssrc: int) -> str:
    """Append `a=ssrc:<ssrc> cname:nimrtc-cname` to the LAST media section
    of `sdp` if it doesn't already contain an `a=ssrc:` line.  Preserves
    CRLF line endings used by RFC 4566.
    """
    # Detect line separator.
    sep = "\r\n" if "\r\n" in sdp else "\n"
    lines = sdp.split(sep)
    # Drop trailing empty entry from split if SDP ended with separator.
    if lines and lines[-1] == "":
        lines.pop()
    media_idxs = [i for i, ln in enumerate(lines) if ln.startswith("m=")]
    if not media_idxs:
        return sdp
    last_media = media_idxs[-1]
    has_ssrc = any(ln.startswith("a=ssrc:") for ln in lines[last_media:])
    if has_ssrc:
        return sdp
    # Insert ssrc right before next m= or append.
    next_m = next((i for i in range(last_media + 1, len(lines))
                   if lines[i].startswith("m=")), None)
    ssrc_line = f"a=ssrc:{ssrc} cname:nimrtc-cname"
    if next_m is not None:
        lines.insert(next_m, ssrc_line)
    else:
        lines.append(ssrc_line)
    return sep.join(lines) + sep


# -----------------------------------------------------------------------------

async def pump_subprocess_to_ws(sub: NimRTCSubprocess, ws,
                                  fingerprint_out: Optional[Path] = None,
                                  inject_ssrc: int = 0) -> None:
    """Forward JSON lines from demo-p2p to the WebSocket.

    If `inject_ssrc` > 0, every answer SDP emitted by demo-p2p gets an
    `a=ssrc:<inject_ssrc> cname:nimrtc-cname` line appended (if not already
    present).  This is required for Chrome interop: Chrome refuses to count
    inbound RTP packets in `inbound-rtp` reports until the answer SDP carries
    at least one `a=ssrc:` line — see RFC 8829 §5.2.1 and the Chromium bug
    where transport stats show packetsReceived=N but the per-stream
    `inbound-rtp` is omitted.

    demo-p2p's current answerer path does not emit a=ssrc (its SDP is
    generated by the engine from offer processing, which has no notion of
    outgoing SSRC since NimRTC does not run RTCRtpSender — it writes raw
    SRTP packets to the transport).  The SSRC value chosen here is a
    deterministic constant; the real SSRC on the wire is whatever demo-p2p's
    encoder stamps into the RTP header.  Chrome will demux by RTP-header
    SSRC, not by SDP-declared SSRC, so the mismatch is harmless and
    fixes the missing inbound-rtp report.

    On _closed (WS side closed or bye received), drain the queue for a
    brief window before exiting so that any answer/candidates already in the
    pipe are flushed to the peer even after pump_ws_to_subprocess exits.
    """
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
        # Capture the answerer's SDP fingerprint after forwarding it.  The
        # value is intentionally JSON so native bridge tools can consume it.
        if fingerprint_out and mtype in ("answer", "sdp"):
            sdp = parsed.get("sdp", "")
            import re
            match = re.search(r"^a=fingerprint:\s*sha-256\s+([^\\r\\n]+)", sdp, re.MULTILINE | re.IGNORECASE)
            if match:
                fingerprint_out.parent.mkdir(parents=True, exist_ok=True)
                fingerprint_out.write_text(json.dumps({"algorithm": "sha-256", "fingerprint": match.group(1).strip()}, indent=2) + "\n", encoding="utf-8")
                logger.info("wrote answer fingerprint to %s", fingerprint_out)
        # Inject a=ssrc into answer SDPs when enabled.  This is a Chrome
        # interop workaround — see the docstring above.
        if inject_ssrc and mtype == "answer" and "sdp" in parsed:
            parsed["sdp"] = _inject_ssrc_into_sdp(parsed["sdp"], inject_ssrc)
            msg = json.dumps(parsed)
            logger.info("injected a=ssrc:%d into answer SDP", inject_ssrc)
        sender = parsed.get("from", "")
        if sender:
            logger.info("demo-p2p >> %s (from %s)", mtype, sender)
        else:
            logger.info("demo-p2p >> %s", mtype)
        try:
            await ws.send(msg)
        except websockets.exceptions.ConnectionClosed:
            logger.warning("WS closed mid-send for %s — draining queue", mtype)
            sub._closed = True
            break

    # Drain any remaining queued messages (e.g. answer + candidates that
    # arrived before the close signal) before this pump exits so Chrome sees
    # the complete answer even when we exit early due to a bye / WS close.
    drained = 0
    while True:
        try:
            msg = await sub.next_message(timeout=0.5)
        except Exception:
            break
        if msg is None:
            break
        drained += 1
        try:
            parsed = json.loads(msg)
        except json.JSONDecodeError:
            logger.warning("drain: non-JSON skipped: %s", msg[:80])
            continue
        mtype = parsed.get("type", "?")
        logger.info("drain: flushing %s", mtype)
        try:
            await ws.send(msg)
        except websockets.exceptions.ConnectionClosed:
            logger.warning("drain: WS closed during flush of %s", mtype)
            break
    if drained:
        logger.info("drain: flushed %d pending messages", drained)


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
    ap.add_argument("--turn-host", default="",
                    help="TURN server hostname (enables TURN relay candidates)")
    ap.add_argument("--turn-port", type=int, default=3478,
                    help="TURN server port (default: 3478)")
    ap.add_argument("--turn-user", default="",
                    help="TURN username")
    ap.add_argument("--turn-pass", default="",
                    help="TURN password")
    ap.add_argument("--fingerprint-out", type=Path,
                    help="Write forwarded answer sha-256 fingerprint as JSON")
    ap.add_argument("--local-fingerprint-in", type=Path,
                    help="Optional local fingerprint JSON for symmetric bridge plumbing")
    ap.add_argument("--inject-ssrc", type=int, default=0,
                    help="If > 0, append a=ssrc:<value> cname:nimrtc-cname to "
                         "the last media section of every forwarded answer SDP "
                         "(required for Chrome interop — see _inject_ssrc_into_sdp)")
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
                           stun_port=args.stun_port, no_stun=args.no_stun,
                           turn_host=args.turn_host, turn_port=args.turn_port,
                           turn_user=args.turn_user, turn_pass=args.turn_pass)
    await sub.start()

    logger.info("Connecting to signaling server: %s", args.signaling)
    try:
        async with websockets.connect(args.signaling) as ws:
            logger.info("WebSocket connected — bridging")
            send_task = asyncio.create_task(pump_ws_to_subprocess(ws, sub))
            recv_task = asyncio.create_task(pump_subprocess_to_ws(
                sub, ws, args.fingerprint_out, args.inject_ssrc))
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
