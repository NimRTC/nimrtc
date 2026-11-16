#!/usr/bin/env python3
"""Example remote-desktop client for the reference signalling service.

Two clients connect through ``signalling.py`` and establish a media
session.  The **viewer** is the offerer; the **device** answers (see
``docs/guides/remote-desktop.md`` §2 for why).

The client is the *signalling* half of the peer.  The media half is a
media binary that speaks newline-delimited JSON on stdin/stdout using
exactly the message shapes this signalling service relays (``offer`` /
``answer`` / ``candidate``).  The client bridges the two: a message that
arrives over the WebSocket is written to the binary's stdin, and a line
the binary writes to stdout is sent over the WebSocket.

Two media binaries are supported, selected with ``--media``:

``demo-p2p``
    The generic WebRTC example (audio + optional video).  This is the
    default and is what the signalling guide demonstrates.

``rd``
    The remote-desktop pair — ``rd_device`` (the machine whose screen is
    shared) and ``rd_viewer`` (the operator's client).  The pair speaks
    the *same* JSON dialect but adds the screen-capture media plane
    (DXGI / WGC → NVENC H.264 → SRTP → NVDEC → sink) and the input
    control plane (the ``rd_input`` wire format over the DTLS
    application-data seam).  ``rd_device`` is always the answerer and
    ``rd_viewer`` always the offerer, so no flag is needed for the role.

If the media binary cannot be found, the client still runs the whole
signalling flow (pairing, join, TURN issuance, candidate relay) and
reports that the media plane was skipped.

## Usage

Terminal 1 (the device — it prints the pairing code):

    python examples/remote-desktop/client.py --role device --url ws://127.0.0.1:8765

Terminal 2 (the operator — it types the pairing code):

    python examples/remote-desktop/client.py --role viewer --code 123456 \\
        --url ws://127.0.0.1:8765

## Usage — the remote-desktop pair

    python examples/remote-desktop/client.py --role device --media rd \\
        --url ws://192.168.1.10:8765

    python examples/remote-desktop/client.py --role viewer --media rd \\
        --code 123456 --url ws://192.168.1.10:8765 --sink d3d11

> The device does **not** inject input unless ``--inject`` is given:
> injecting real mouse and keyboard events into the developer's own
> desktop is not something a test harness should do by surprise.
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
from pathlib import Path
from typing import Dict, List, Optional, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import wsproto  # noqa: E402  (path shim above)

#: Message types that belong to the media plane and are relayed between
#: the two peers.
MEDIA_TYPES = ("offer", "answer", "candidate", "ice_restart", "reconnect")

#: The media binaries the client knows how to drive.
MEDIA_KINDS = ("demo-p2p", "rd")


def _locate(name: str, explicit: Optional[str]) -> Optional[str]:
    """Locate a media binary by name, searching the usual build trees.

    The explicit path wins.  Otherwise every candidate found under
    ``build/`` is collected and the **most recently modified** one is
    returned.

    Picking the newest rather than the first hit matters: this repository
    keeps several build trees alive at once (``build/msvc-full``,
    ``build/ci-msvc``, ``build/zcs``, …) and a stale binary in one of
    them will happily connect, negotiate and then fail in confusing ways
    — e.g. a media plane that never decodes a frame because the fix for
    it landed in a *different* tree.  "Newest wins" makes the harness
    always exercise the code that was actually just compiled.

    Both the flat layout (``<build>/examples/rd_device.exe``) and the
    per-target layout (``<build>/examples/rd_device/rd_device.exe``) are
    supported, because the two exist side by side depending on which
    CMake generator produced the tree.
    """
    if explicit:
        if os.path.isfile(explicit):
            # An explicit *file* only satisfies the request when it is the
            # binary that was asked for.  The harness passes one path to
            # both roles, so a path pointing at ``rd_device.exe`` must not
            # be handed to the viewer — that would run the device binary
            # twice and produce a session with two devices in it.  In that
            # case the explicit path is treated as a *directory* hint and
            # the right binary is picked out of it.
            if os.path.splitext(os.path.basename(explicit))[0].lower() == name.lower():
                return explicit
            sibling = os.path.join(os.path.dirname(explicit), name + (
                ".exe" if os.name == "nt" else ""))
            return sibling if os.path.isfile(sibling) else None
        if os.path.isdir(explicit):
            sibling = os.path.join(explicit, name + (
                ".exe" if os.name == "nt" else ""))
            return sibling if os.path.isfile(sibling) else None
        return None

    exe = ".exe" if os.name == "nt" else ""
    target = name + exe

    found: List[str] = []

    def _add(path: str) -> None:
        if os.path.isfile(path) and path not in found:
            found.append(path)

    # The documented build trees, in both layouts.
    build_root = Path(__file__).resolve().parent.parent.parent / "build"
    for tree in sorted(build_root.glob("*")) if build_root.is_dir() else []:
        if not tree.is_dir():
            continue
        _add(str(tree / "examples" / target))
        _add(str(tree / "examples" / name / target))
        _add(str(tree / "examples" / "Release" / target))
        _add(str(tree / "examples" / "Debug" / target))

    # A recursive sweep catches any other layout (custom build dirs,
    # multi-config generators, …).
    if build_root.is_dir():
        for found_path in build_root.rglob(target):
            _add(str(found_path))

    if found:
        # Newest first: the tree that was compiled most recently holds
        # the code under test.
        found.sort(key=lambda p: os.path.getmtime(p), reverse=True)
        return found[0]

    return shutil.which(target) or None


def find_demo_p2p(explicit: Optional[str]) -> Optional[str]:
    """Locate the ``demo-p2p`` binary, if it has been built."""
    return _locate("demo-p2p", explicit)


def find_rd_binary(role: str, explicit: Optional[str]) -> Optional[str]:
    """Locate the remote-desktop binary for `role` ("device" / "viewer")."""
    return _locate("rd_device" if role == "device" else "rd_viewer", explicit)


def parse_turn_url(url: str) -> Tuple[str, int]:
    """Split a ``turn:host:port?transport=udp`` URL into (host, port)."""
    body = url.split(":", 1)[1] if ":" in url else url
    body = body.split("?", 1)[0]
    host, _, port = body.partition(":")
    try:
        return host, int(port)
    except ValueError:
        return host, 3478


class MediaBridge:
    """Runs a media binary and bridges its JSON lines to the WebSocket.

    Two binaries are supported, and they speak the *same* JSON dialect
    (newline-delimited ``{"type": ..., ...}`` on stdin/stdout):

    ``demo-p2p``
        ``--signaling-proxy [--answerer]``.  The generic WebRTC example.

    ``rd_device`` / ``rd_viewer``
        ``--duration <sec>`` and the role-specific flags.  The roles are
        fixed by the binaries themselves — ``rd_device`` is the answerer
        and ``rd_viewer`` is the offerer — so the bridge only has to pick
        the right binary.

    The bridge is *transparent*: the signalling message **is** the media
    message.  Nothing is interpreted here beyond the framing.
    """

    def __init__(self, kind: str, binary: str, answerer: bool,
                 turn: Optional[Dict], use_turn: bool = False,
                 duration: int = 120, extra: Optional[List[str]] = None) -> None:
        self.kind = kind
        self.binary = binary
        self.answerer = answerer
        self.turn = turn
        self.use_turn = use_turn
        self.duration = duration
        self.extra = list(extra or [])
        self.process: Optional[subprocess.Popen] = None
        self._to_ws: "asyncio.Queue[str]" = asyncio.Queue()
        self._exit_reported = False
        #: True once `stop()` was called — the process was torn down on
        #: purpose, so its exit does not end the session.
        self.expected_exit = False

    def _turn_flags(self) -> List[str]:
        """The TURN flags, or an empty list when TURN is not in use.

        The credential is only passed on when the operator asked for it:
        the signalling service issues one unconditionally, but pointing
        the ICE agent at a TURN server that is not running only makes
        the gathering round slower.
        """
        if not (self.turn and self.use_turn):
            return []
        host, port = parse_turn_url(self.turn.get("url", ""))
        if self.kind == "demo-p2p":
            return [
                "--turn-host", host,
                "--turn-port", str(port),
                "--turn-user", self.turn.get("username", ""),
                "--turn-pass", self.turn.get("password", ""),
            ]
        # rd_device / rd_viewer use the shorter spelling.
        return [
            "--turn", f"{host}:{port}",
            "--turn-user", self.turn.get("username", ""),
            "--turn-pass", self.turn.get("password", ""),
        ]

    def start(self) -> None:
        if self.kind == "demo-p2p":
            cmd = [self.binary, "--signaling-proxy", "--duration", str(self.duration)]
            if self.answerer:
                cmd.append("--answerer")
        else:
            cmd = [self.binary, "--duration", str(self.duration)]
        cmd += self._turn_flags()
        cmd += self.extra
        self.process = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=None,  # let the binary write to the console
            text=True,
            bufsize=1,
        )

    async def wait_done(self) -> int:
        """Wait until the media binary exits, and return its exit code.

        The media binary's lifetime is the *session's* lifetime: when it
        exits — because `--duration` ran out, or because it crashed — the
        whole client is done, signalling channel included.  Waiting on
        the process here is what lets the caller tear the session down
        instead of leaving a live socket behind (see `run_client`).
        """
        if self.process is None:
            return 0
        loop = asyncio.get_event_loop()
        return await loop.run_in_executor(None, self.process.wait)

    def stop(self) -> None:
        if self.process is None:
            return
        self.expected_exit = True
        try:
            if self.process.stdin:
                self.process.stdin.close()
        except (OSError, ValueError):
            pass
        # Give the binary a moment to shut down on its own.  Closing its
        # stdin is the signal it uses to notice that the signalling
        # channel is gone, and a graceful exit lets it print its
        # "state: closed" marker — which is what the harness (and the
        # operator reading the log) looks for.
        try:
            self.process.wait(timeout=3.0)
        except subprocess.TimeoutExpired:
            try:
                self.process.terminate()
                self.process.wait(timeout=5)
            except (OSError, subprocess.TimeoutExpired):
                try:
                    self.process.kill()
                except OSError:
                    pass
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
                # EOF: the binary closed its stdout.  Report *why* it
                # stopped — a media binary that dies on a signal (a
                # crash, an access violation) would otherwise look
                # exactly like a clean exit, and the operator would be
                # left staring at a silent disconnect.
                self.report_exit()
                return
            line = line.strip()
            if not line:
                continue
            try:
                message = json.loads(line)
            except json.JSONDecodeError:
                continue
            on_message(message)

    def report_exit(self) -> None:
        """Print the media binary's exit status, once."""
        if self._exit_reported:
            return
        self._exit_reported = True
        proc = self.process
        if proc is None:
            return
        try:
            code = proc.poll()
        except OSError:
            return
        if code is None:
            return
        if code == 0:
            print(f"[media] {os.path.basename(self.binary)} exited cleanly (code 0)")
            return
        # A negative code on POSIX is a signal; on Windows the raw
        # NTSTATUS comes through (0xC0000005 = access violation,
        # 0xC000007B = DLL not found).  Name the two that actually
        # bite in practice.
        hint = ""
        if code == -1073741515 or code == 0xC000007B:
            hint = " — STATUS_DLL_NOT_FOUND: a required DLL is missing " \
                   "(check that msquic.dll sits next to the exe)"
        elif code == -1073741819 or code == 0xC0000005:
            hint = " — STATUS_ACCESS_VIOLATION: the binary crashed"
        print(f"[media] {os.path.basename(self.binary)} exited with code "
              f"{code} (0x{code & 0xFFFFFFFF:08X}){hint}", file=sys.stderr)


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

    bridge: Optional[MediaBridge] = None
    stdout_task: Optional[asyncio.Future] = None
    watcher: Optional[asyncio.Future] = None
    pending: List[Dict] = []
    peer_present = False
    state: Dict[str, object] = {}

    # ---- The media binary's exit ends the session -------------------------
    #
    # `--duration` is the *media* session's lifetime: when it runs out,
    # the media binary exits but the signalling socket would stay open,
    # and the client would sit there forever.  That is not a harmless
    # leak:
    #
    #   - the operator sees a console that looks alive but shows nothing,
    #     so the natural reaction is to press Ctrl+C and start over;
    #   - the device never sees a `peer_left`, so it never asks for a
    #     fresh pairing code;
    #   - the pairing code the operator used stays *spent*, and the next
    #     run with that code is refused with `pairing_invalid`.
    #
    # So the media binary's exit tears the whole client down: the socket
    # closes, the device is told the operator left, and it hands out a
    # new code.  The operator's next run then works.
    media_exited = asyncio.Event()

    async def send(message: Dict) -> None:
        await conn.send_text(json.dumps(message))

    def build_media() -> Optional[MediaBridge]:
        """Build the media bridge for this role (without starting it)."""
        if args.media == "rd":
            # The remote-desktop pair: the binary *is* the role, so the
            # role's binary is picked and no offerer/answerer flag is
            # needed — rd_viewer is always the offerer and rd_device is
            # always the answerer.
            binary = find_rd_binary(args.role, args.binary)
            if binary is None:
                print(f"[media] rd_{args.role} binary not found - media plane "
                      f"skipped (build it, or pass --binary)")
                return None
            extra: List[str] = []
            if args.media == "rd" and args.role == "viewer":
                if args.sink:
                    extra += ["--sink", args.sink]
                if args.codec:
                    extra += ["--codec", args.codec]
                if args.backend:
                    extra += ["--backend", args.backend]
                if args.no_input:
                    extra.append("--no-input")
                if args.no_qoe:
                    extra.append("--no-qoe")
            if args.media == "rd" and args.role == "device":
                if args.codec:
                    extra += ["--codec", args.codec]
                if args.backend:
                    extra += ["--backend", args.backend]
                if args.capture:
                    extra += ["--capture", args.capture]
                if not args.inject:
                    # Injecting real mouse and keyboard events into the
                    # developer's own desktop is not something a test
                    # harness should do by surprise.
                    extra.append("--no-inject")
            if args.no_control:
                extra.append("--no-control")
            if args.no_stun:
                # A LAN-only run: no STUN gathering at all, so the ICE
                # agent gathers host candidates only.  This is what a
                # two-machine test on a private network wants — reaching
                # out to a public STUN server adds a server-reflexive
                # candidate that can win the pair selection and then
                # break the session the moment the NAT mapping moves.
                extra.append("--no-stun")
            return MediaBridge("rd", binary, answerer=(args.role == "device"),
                               turn=state.get("turn"), use_turn=args.use_turn,
                               duration=args.duration, extra=extra)
        binary = find_demo_p2p(args.binary)
        if binary is None:
            print("[media] demo-p2p binary not found - media plane skipped "
                  "(build it, or pass --binary)")
            return None
        return MediaBridge("demo-p2p", binary, answerer=(args.role == "device"),
                           turn=state.get("turn"), use_turn=args.use_turn,
                           duration=args.duration)

    async def watch_media(b: MediaBridge) -> None:
        """Wait for the media binary to exit on its own."""
        await b.wait_done()
        if b.expected_exit:
            # We stopped it deliberately (the peer changed) — that is not
            # the end of the client.
            return
        media_exited.set()

    def start_media() -> None:
        """Start the media binary and pump its stdout to the signalling channel.

        A *fresh* media binary is built for every session.  That matters
        for the device: the media plane carries a DTLS-SRTP session, and
        a DTLS session belongs to **one** peer.  When the operator leaves
        and comes back, the device must not keep the old session around
        — the new operator's DTLS handshake has to run against a fresh
        one, or the two sides derive different SRTP keys and the operator
        sees a session that connects but never decodes a frame.
        """
        nonlocal bridge, stdout_task, watcher
        new_bridge = build_media()
        if new_bridge is None:
            return

        new_bridge.start()
        # The stdout pump has to start *with* the binary: the answer and
        # the local candidates are written to stdout as soon as the
        # binary has them, and a pump that starts later would lose them.
        new_stdout_task = asyncio.ensure_future(
            new_bridge.pump_stdout(lambda m: asyncio.ensure_future(send(m)))
        )
        new_watcher = asyncio.ensure_future(watch_media(new_bridge))
        # A message that arrived before the media plane existed is
        # forwarded now that it does.  The queue holds *every* message,
        # not just the last one: the candidates arrive in a burst right
        # after the SDP, and a single slot would drop all but the last.
        nonlocal pending
        for message in pending:
            new_bridge.write(message)
        pending = []
        bridge = new_bridge
        stdout_task = new_stdout_task
        watcher = new_watcher
        media_exited.clear()
        print(f"[media] started {new_bridge.binary} "
              f"(answerer={args.role == 'device'})")

    def stop_media() -> None:
        """Tear the media plane down (the peer left, or a new one arrived)."""
        nonlocal bridge, stdout_task, watcher
        if watcher is not None:
            watcher.cancel()
            watcher = None
        if stdout_task is not None:
            stdout_task.cancel()
            stdout_task = None
        if bridge is not None:
            bridge.stop()
            bridge = None

    def maybe_start_media() -> None:
        """Start (or restart) the media binary once both peers are in the room.

        The restart is the point: a device that already served one
        operator has to serve the next one with a *fresh* media session,
        because the DTLS-SRTP session is per-peer.
        """
        nonlocal peer_present
        if not peer_present:
            return
        if bridge is not None:
            print(f"[media] the peer changed - restarting the media plane")
            stop_media()
        start_media()

    async def handle(message: Dict) -> None:
        nonlocal pending, peer_present
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
            # The media session belonged to the peer that just left: its
            # DTLS-SRTP session, its SRTP keys, its decoder state.  Tear
            # the media plane down so the next peer gets a fresh one.
            stop_media()
            # The operator is gone, so the pairing code that let them
            # in has been consumed.  The device has to ask for a fresh
            # one now: the next operator cannot use the old code, and a
            # device that has no live code cannot admit anybody at all.
            if args.role == "device":
                print(f"[{args.role}] the operator left - requesting a new "
                      f"pairing code")
                await send({"type": "pairing_request"})
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
            # A device whose code was consumed (or has expired) is
            # stranded: it cannot admit an operator any more.  Ask for a
            # fresh code instead of sitting there useless.
            if (args.role == "device" and
                    message.get("code") == "pairing_invalid"):
                print(f"[{args.role}] the pairing code is spent - "
                      f"requesting a new one")
                await send({"type": "pairing_request"})
        elif mtype in MEDIA_TYPES:
            # A media-plane message: the SDP / candidate / restart is
            # handed to the media binary.  The bridge is transparent —
            # the signalling message *is* the media message.
            print(f"[{args.role}] <- {mtype}")
            if bridge is not None:
                bridge.write(message)
            else:
                pending.append(message)
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
    media_waiter = asyncio.ensure_future(media_exited.wait())

    try:
        await asyncio.wait(
            {ws_task, media_waiter},
            return_when=asyncio.FIRST_COMPLETED,
        )
    except (KeyboardInterrupt, asyncio.CancelledError):
        pass
    except wsproto.ConnectionClosed:
        print(f"[{args.role}] the signalling channel closed")
    finally:
        ws_task.cancel()
        media_waiter.cancel()
        if watcher is not None:
            watcher.cancel()
        if stdout_task is not None:
            stdout_task.cancel()
        if bridge is not None:
            bridge.stop()
        await conn.close()
    return 0


def main() -> int:
    # The client prints the pairing code, and the operator has to be able
    # to read it off the console.  Python block-buffers stdout when it is
    # not a terminal (a pipe, a CI log), so without this the code would
    # not appear until the process exited.
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except (AttributeError, OSError):
        pass

    parser = argparse.ArgumentParser(description="NimRTC remote-desktop example client")
    parser.add_argument("--role", choices=("device", "viewer"), required=True,
                        help="which side of the session this client is")
    parser.add_argument("--url", default="ws://127.0.0.1:8765",
                        help="signalling service URL (default: ws://127.0.0.1:8765)")
    parser.add_argument("--code", default="",
                        help="the pairing code (viewer role)")
    parser.add_argument("--binary", default=None,
                        help="path to the media binary (default: search build/)")
    parser.add_argument("--media", choices=MEDIA_KINDS, default="demo-p2p",
                        help="which media binary to drive: demo-p2p (the "
                             "generic WebRTC example) or rd (the "
                             "remote-desktop pair)")
    parser.add_argument("--duration", type=int, default=120,
                        help="how long the media session may run, in seconds")
    parser.add_argument("--use-turn", action="store_true",
                        help="pass the issued TURN credential to the media binary "
                             "(only useful when a TURN server is actually running)")
    # ---- The remote-desktop pair's own flags (--media rd) ----------------
    parser.add_argument("--sink", default=None,
                        help="rd_viewer: the video sink (headless | d3d11)")
    parser.add_argument("--codec", default=None,
                        help="rd_*: the video codec (h264 | av1 | h265)")
    parser.add_argument("--backend", default=None,
                        help="rd_*: the codec backend (auto | hardware | software)")
    parser.add_argument("--capture", default=None,
                        help="rd_device: the capture backend "
                             "(windows-wgc | windows-dxgi)")
    parser.add_argument("--inject", action="store_true",
                        help="rd_device: let the device inject the received "
                             "input into its own desktop (off by default)")
    parser.add_argument("--no-input", action="store_true",
                        help="rd_viewer: watch only; do not send any input")
    parser.add_argument("--no-qoe", action="store_true",
                        help="rd_viewer: do not collect the QoE metrics")
    parser.add_argument("--no-control", action="store_true",
                        help="rd_*: do not open a control channel (media only)")
    parser.add_argument("--no-stun", action="store_true",
                        help="rd_*: do not gather STUN candidates (LAN only)")
    args = parser.parse_args()
    try:
        return asyncio.run(run_client(args))
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
