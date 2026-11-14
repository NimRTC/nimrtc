#!/usr/bin/env python3
"""Example remote-desktop client for the reference signalling service.

Two clients connect through ``signalling.py`` and establish a media
session.  The **viewer** is the offerer; the **device** answers (see
``docs/guides/remote-desktop.md`` §2 for why).

The client is the *signalling* half of the peer.  The media half is the
existing ``demo-p2p`` example binary, which speaks newline-delimited
JSON on stdin/stdout using exactly the message shapes this signalling
service relays (``offer`` / ``answer`` / ``candidate``).  The client
bridges the two: a message that arrives over the WebSocket is written
to the binary's stdin, and a line the binary writes to stdout is sent
over the WebSocket.

If the ``demo-p2p`` binary cannot be found, the client still runs the
whole signalling flow (pairing, join, TURN issuance, candidate relay)
and reports that the media plane was skipped.

## Usage

Terminal 1 (the device — it prints the pairing code):

    python examples/remote-desktop/client.py --role device --url ws://127.0.0.1:8765

Terminal 2 (the operator — it types the pairing code):

    python examples/remote-desktop/client.py --role viewer --code 123456 \\
        --url ws://127.0.0.1:8765
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import shutil
import subprocess
import sys
import time
from typing import Dict, Optional, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import wsproto  # noqa: E402  (path shim above)

#: Message types that belong to the media plane and are relayed between
#: the two peers.
MEDIA_TYPES = ("offer", "answer", "candidate", "ice_restart", "reconnect")


def find_demo_p2p(explicit: Optional[str]) -> Optional[str]:
    """Locate the ``demo-p2p`` binary, if it has been built."""
    if explicit:
        return explicit if os.path.exists(explicit) else None
    exe = ".exe" if os.name == "nt" else ""
    candidates = [
        os.path.join("build", "msvc-full", "examples", "demo-p2p", "demo-p2p" + exe),
        os.path.join("build", "examples", "Release", "demo-p2p" + exe),
        os.path.join("build", "examples", "Debug", "demo-p2p" + exe),
        os.path.join("build", "examples", "demo-p2p", "demo-p2p" + exe),
        shutil.which("demo-p2p" + exe) or "",
    ]
    for candidate in candidates:
        if candidate and os.path.exists(candidate):
            return candidate
    return None


def parse_turn_url(url: str) -> Tuple[str, int]:
    """Split a ``turn:host:port?transport=udp`` URL into (host, port)."""
    body = url.split(":", 1)[1] if ":" in url else url
    body = body.split("?", 1)[0]
    host, _, port = body.partition(":")
    try:
        return host, int(port)
    except ValueError:
        return host, 3478


class DemoP2PBridge:
    """Runs ``demo-p2p`` and bridges its JSON lines to the WebSocket."""

    def __init__(self, binary: str, answerer: bool, turn: Optional[Dict],
                 use_turn: bool = False) -> None:
        self.binary = binary
        self.answerer = answerer
        self.turn = turn
        self.use_turn = use_turn
        self.process: Optional[subprocess.Popen] = None
        self._to_ws: "asyncio.Queue[str]" = asyncio.Queue()

    def start(self) -> None:
        cmd = [self.binary, "--signaling-proxy", "--duration", "120"]
        if self.answerer:
            cmd.append("--answerer")
        if self.turn and self.use_turn:
            # Only pass the TURN configuration when the operator asked for
            # it.  The signalling service issues the credential
            # unconditionally, but pointing the ICE agent at a TURN server
            # that is not running only makes the gathering round slower.
            host, port = parse_turn_url(self.turn.get("url", ""))
            cmd += [
                "--turn-host", host,
                "--turn-port", str(port),
                "--turn-user", self.turn.get("username", ""),
                "--turn-pass", self.turn.get("password", ""),
            ]
        self.process = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=None,  # let the demo write to the console
            text=True,
            bufsize=1,
        )

    def stop(self) -> None:
        if self.process is None:
            return
        try:
            if self.process.stdin:
                self.process.stdin.close()
            self.process.terminate()
            self.process.wait(timeout=5)
        except (OSError, subprocess.TimeoutExpired):
            try:
                self.process.kill()
            except OSError:
                pass
        self.process = None

    def write(self, message: Dict) -> None:
        """Forward a message from the signalling channel to the binary."""
        if self.process is None or self.process.stdin is None:
            return
        try:
            self.process.stdin.write(json.dumps(message) + "\n")
            self.process.stdin.flush()
        except (OSError, ValueError):
            pass

    async def pump_stdout(self, on_message) -> None:
        """Read the binary's stdout and hand each JSON line to ``on_message``."""
        if self.process is None or self.process.stdout is None:
            return
        loop = asyncio.get_event_loop()
        while True:
            line = await loop.run_in_executor(None, self.process.stdout.readline)
            if not line:
                return
            line = line.strip()
            if not line:
                continue
            try:
                message = json.loads(line)
            except json.JSONDecodeError:
                continue
            on_message(message)


async def run_client(args: argparse.Namespace) -> int:
    url = args.url
    if not url.startswith("ws://"):
        print(f"error: --url must be a ws:// URL (got {url!r})", file=sys.stderr)
        return 64
    rest = url[len("ws://"):]
    hostport, _, path = rest.partition("/")
    path = "/" + path if path else "/"
    host, _, port_text = hostport.partition(":")
    port = int(port_text) if port_text else 8765

    conn = await wsproto.client_connect(host, port, path)
    print(f"[{args.role}] connected to {url}")

    bridge: Optional[DemoP2PBridge] = None
    stdout_task: Optional[asyncio.Future] = None
    pending_offer: Optional[Dict] = None
    peer_present = False
    state: Dict[str, object] = {}

    async def send(message: Dict) -> None:
        await conn.send_text(json.dumps(message))

    def maybe_start_media() -> None:
        """Start ``demo-p2p`` once both peers are in the room."""
        nonlocal bridge, stdout_task
        if bridge is not None or not peer_present:
            return
        binary = find_demo_p2p(args.binary)
        if binary is None:
            print("[media] demo-p2p binary not found - media plane skipped "
                  "(build it, or pass --binary)")
            return
        bridge = DemoP2PBridge(binary, answerer=(args.role == "device"),
                               turn=state.get("turn"), use_turn=args.use_turn)
        bridge.start()
        # The stdout pump has to start *with* the binary: the answer and
        # the local candidates are written to stdout as soon as the
        # binary has them, and a pump that starts later would lose them.
        stdout_task = asyncio.ensure_future(
            bridge.pump_stdout(lambda m: asyncio.ensure_future(send(m)))
        )
        # A message that arrived before the media plane existed is
        # forwarded now that it does.
        nonlocal pending_offer
        if pending_offer is not None:
            bridge.write(pending_offer)
            pending_offer = None
        print(f"[media] started {binary} (answerer={args.role == 'device'})")

    async def handle(message: Dict) -> None:
        nonlocal pending_offer, peer_present
        mtype = message.get("type")
        if mtype == "pairing_code":
            remaining = int(message["expires_at"] - time.time())
            print(f"[{args.role}] pairing code: {message['code']} "
                  f"(valid for {remaining} s)")
            await send({"type": "device_register", "code": message["code"]})
        elif mtype == "device_registered":
            state["credential"] = message["credential"]
            state["room_id"] = message["room_id"]
            print(f"[{args.role}] registered; room={message['room_id']}")
            await send({"type": "join", "credential": message["credential"]})
        elif mtype == "paired":
            state["credential"] = message["credential"]
            state["room_id"] = message["room_id"]
            print(f"[{args.role}] paired; room={message['room_id']}")
            await send({"type": "join", "credential": message["credential"]})
        elif mtype == "joined":
            peer_present = bool(message.get("peer_present"))
            print(f"[{args.role}] joined room={message.get('room_id')} "
                  f"as {message.get('role')} (peer_present={peer_present})")
            maybe_start_media()
        elif mtype == "peer_joined":
            peer_present = True
            print(f"[{args.role}] the {message.get('role')} joined")
            maybe_start_media()
        elif mtype == "peer_left":
            peer_present = False
            print(f"[{args.role}] the {message.get('role')} left")
        elif mtype == "turn_credentials":
            state["turn"] = {
                "url": message["url"],
                "username": message["username"],
                "password": message["password"],
            }
            print(f"[{args.role}] TURN credential: {message['username']} "
                  f"(ttl {int(message['ttl'])} s)")
        elif mtype == "error":
            print(f"[{args.role}] ERROR {message.get('code')}: {message.get('message')}")
        elif mtype in MEDIA_TYPES:
            # A media-plane message: the SDP / candidate / restart is
            # handed to the media binary.  The bridge is transparent —
            # the signalling message *is* the media message.
            print(f"[{args.role}] <- {mtype}")
            if bridge is not None:
                bridge.write(message)
            else:
                pending_offer = message
        else:
            print(f"[{args.role}] <- {mtype}")

    async def pump_ws() -> None:
        while True:
            raw = await conn.recv_text()
            await handle(json.loads(raw))

    # ---- the signalling flow ---------------------------------------------

    if args.role == "device":
        await send({"type": "pairing_request"})
    else:
        if not args.code:
            print("error: --role viewer needs --code <pairing code>", file=sys.stderr)
            return 64
        await send({"type": "pair", "code": args.code})

    # Ask for a TURN credential before joining, so the ICE transport can
    # be configured with it before candidate gathering starts.  The
    # credential is only *used* when --use-turn is passed, because the
    # default TURN URL points at a server that is usually not running.
    await send({"type": "turn_request"})

    ws_task = asyncio.ensure_future(pump_ws())

    try:
        await ws_task
    except (KeyboardInterrupt, asyncio.CancelledError):
        pass
    except wsproto.ConnectionClosed:
        print(f"[{args.role}] the signalling channel closed")
    finally:
        ws_task.cancel()
        if stdout_task is not None:
            stdout_task.cancel()
        if bridge is not None:
            bridge.stop()
        await conn.close()
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="NimRTC remote-desktop example client")
    parser.add_argument("--role", choices=("device", "viewer"), required=True,
                        help="which side of the session this client is")
    parser.add_argument("--url", default="ws://127.0.0.1:8765",
                        help="signalling service URL (default: ws://127.0.0.1:8765)")
    parser.add_argument("--code", default="",
                        help="the pairing code (viewer role)")
    parser.add_argument("--binary", default=None,
                        help="path to the demo-p2p binary (default: search build/)")
    parser.add_argument("--duration", type=int, default=120,
                        help="how long the media session may run, in seconds")
    parser.add_argument("--use-turn", action="store_true",
                        help="pass the issued TURN credential to the media binary "
                             "(only useful when a TURN server is actually running)")
    args = parser.parse_args()
    try:
        return asyncio.run(run_client(args))
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
