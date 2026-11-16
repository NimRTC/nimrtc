#!/usr/bin/env python3
"""Drive the reference signalling service with two local clients.

This is the *cross-machine* shape, run on one machine: the device client
and the viewer client each run as their own process, connected to the
WebSocket signalling service over a real TCP socket, and the media
plane runs between the two as two separate processes that talk over real
UDP sockets through ICE.

The only difference from a genuine two-machine run is the loopback
address in the signalling URL — everything else (the WebSocket relay,
the ICE/DTLS/SRTP handshake, the capture/encode/decode path) is the
real thing.
"""

import argparse
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
#: The signalling service and the client live in the example directory,
#: not next to this script.
EXAMPLE_DIR = os.path.abspath(os.path.join(HERE, "..", "examples", "remote-desktop"))


def start_service(port: int, host: str = "127.0.0.1"):
    return subprocess.Popen(
        [sys.executable, os.path.join(EXAMPLE_DIR, "signalling.py"),
         "--host", host, "--port", str(port)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )


def start_client(role: str, url: str, extra, log_path: str):
    cmd = [sys.executable, os.path.join(EXAMPLE_DIR, "client.py"),
           "--role", role, "--url", url] + extra
    # stdout and stderr both go to the log: the pairing code is printed
    # on stdout and the media binary's diagnostics on stderr.
    #
    # PYTHONUNBUFFERED matters: without it Python block-buffers stdout
    # when it is not a terminal, and the pairing code would sit in the
    # buffer until the process exited — the harness would never see it.
    env = dict(os.environ, PYTHONUNBUFFERED="1")
    log = open(log_path, "w", encoding="utf-8")
    return subprocess.Popen(cmd, stdout=log, stderr=log, text=True, env=env)


def read_pairing_code(log_path: str, timeout: float = 15.0) -> str:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with open(log_path, "r", encoding="utf-8") as fh:
                text = fh.read()
        except OSError:
            text = ""
        match = re.search(r"pairing code:\s*(\d{6})", text)
        if match:
            return match.group(1)
        time.sleep(0.3)
    raise SystemExit("the device never printed a pairing code")


def wait_for(proc, timeout: float) -> int:
    """Wait for a child, killing it if it overruns — and never raising.

    `subprocess.wait` raises `TimeoutExpired` when the child outlives the
    deadline, which used to abort the harness *before* the logs were
    dumped — and the logs are the whole point of a failing run.  A client
    that hangs (a device that never opens, a viewer stuck in a loop) is a
    result, not a crash: it is reported as a non-zero exit code so the
    verdict below still runs.
    """
    try:
        return proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.terminate()
        try:
            return proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            return -1


#: The marker the media binaries print when the session is over.  The
#: signalling clients keep running after this (the WebSocket stays open),
#: so the marker — not the client's exit code — is what ends the run.
MEDIA_END_MARKERS = ("state: closed", "engine open failed",
                     "error 0x", "the signalling channel closed")


def wait_for_media_end(proc, log_path: str, timeout: float) -> int:
    """Wait until the *media* session is over, then stop the client.

    Returns 0 when the media session ended normally, and a non-zero code
    when the client died early or the session never came up.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            # The client is gone.  It is a clean end only if the media
            # session had reached its end.
            return 0 if media_session_finished(log_path) else (proc.returncode or 1)
        if media_session_finished(log_path):
            return 0
        time.sleep(0.3)
    return wait_for(proc, 1)


def media_session_finished(log_path: str) -> bool:
    """Has the media session reached its end (or died) according to the log?"""
    try:
        with open(log_path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError:
        return False
    return any(marker in text for marker in MEDIA_END_MARKERS)


def read_viewer_stats(log_path: str):
    """Extract the last ``[rd_viewer] frames=… decoded=…`` statistics line.

    The viewer's periodic report is the only place the media plane's
    outcome is visible, so the harness parses it instead of guessing from
    the exit codes.
    """
    try:
        with open(log_path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError:
        return None

    pattern = re.compile(
        r"frames=(\d+)\s+decoded=(\d+)\s+rendered=(\d+)\s+dropped=(\d+)")
    matches = pattern.findall(text)
    if not matches:
        return None
    last = matches[-1]
    return {
        "frames": int(last[0]),
        "decoded": int(last[1]),
        "rendered": int(last[2]),
        "dropped": int(last[3]),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--media", default="rd",
                        help="demo-p2p | rd")
    parser.add_argument("--duration", type=int, default=10)
    parser.add_argument("--port", type=int, default=8766)
    parser.add_argument("--binary", default=None)
    parser.add_argument("--inject", action="store_true")
    parser.add_argument("--sink", default=None)
    parser.add_argument("--capture", default=None)
    parser.add_argument("--backend", default=None,
                        help="the codec backend: auto | hardware | software")
    parser.add_argument("--no-stun", action="store_true",
                        help="gather host candidates only (the LAN-only mode: "
                             "no server-reflexive candidate from a public "
                             "STUN server)")
    parser.add_argument("--bind", default="127.0.0.1",
                        help="the address the signalling service binds to, and "
                             "the address the clients connect to. Use the "
                             "LAN address (e.g. 192.168.1.20) to exercise the "
                             "cross-network shape instead of the loopback")
    args = parser.parse_args()

    # HERE is <repo>/tools, so the repository root is one level up and
    # the build tree is a sibling of it.
    log_dir = os.path.abspath(os.path.join(HERE, "..", "build"))
    os.makedirs(log_dir, exist_ok=True)
    device_log = os.path.join(log_dir, "lan_device.log")
    viewer_log = os.path.join(log_dir, "lan_viewer.log")

    url = f"ws://{args.bind}:{args.port}"
    service = start_service(args.port, args.bind)
    time.sleep(1.5)

    device_extra = ["--media", args.media, "--duration", str(args.duration)]
    if args.no_stun:
        device_extra.append("--no-stun")
    if args.binary:
        device_extra += ["--binary", args.binary]
    if args.capture:
        device_extra += ["--capture", args.capture]
    if args.backend:
        device_extra += ["--backend", args.backend]
    if args.inject:
        device_extra.append("--inject")

    device = start_client("device", url, device_extra, device_log)
    try:
        code = read_pairing_code(device_log)

        viewer_extra = ["--media", args.media, "--duration", str(args.duration),
                        "--code", code]
        if args.no_stun:
            viewer_extra.append("--no-stun")
        if args.binary:
            viewer_extra += ["--binary", args.binary]
        if args.sink:
            viewer_extra += ["--sink", args.sink]
        if args.backend:
            viewer_extra += ["--backend", args.backend]

        viewer = start_client("viewer", url, viewer_extra, viewer_log)

        # The *media* session has its own lifetime (--duration) and it is
        # the thing under test.  The signalling clients outlive it: they
        # keep the WebSocket open until the peer disappears, so waiting for
        # the client processes to exit would only measure the harness's
        # own patience.
        #
        # So the run ends when the media session is over — either because
        # the duration elapsed (both binaries print their final state
        # line) or because a client died early.
        device_rc = wait_for_media_end(device, device_log,
                                       args.duration + 25)
        viewer_rc = wait_for_media_end(viewer, viewer_log, 25)
    finally:
        service.terminate()
        try:
            service.wait(timeout=5)
        except subprocess.TimeoutExpired:
            service.kill()

    # The clients are terminated on the way out; that is expected and is
    # not a failure of the session.
    for proc in (device, viewer):
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()

    # ---- The verdict ------------------------------------------------------
    #
    # The exit codes alone are not a verdict: the device and the viewer
    # both exit cleanly even when the media plane never produced a
    # picture.  What the run is *for* is the media plane, so the harness
    # asserts on the decoded-frame counters.
    stats = read_viewer_stats(viewer_log)

    print("=" * 70)
    print("--- device log ---")
    with open(device_log, "r", encoding="utf-8") as fh:
        print(fh.read()[-4000:])
    print("--- viewer log ---")
    with open(viewer_log, "r", encoding="utf-8") as fh:
        print(fh.read()[-4000:])
    print("=" * 70)
    print(f"rc device={device_rc} viewer={viewer_rc}")

    ok = device_rc == 0 and viewer_rc == 0
    if stats is None:
        print("FAIL: the viewer never reported a frame statistics line")
        ok = False
    else:
        decoded = stats.get("decoded", 0)
        rendered = stats.get("rendered", 0)
        dropped = stats.get("dropped", 0)
        print(f"media plane: decoded={decoded} rendered={rendered} dropped={dropped}")
        if decoded <= 0:
            print("FAIL: no frame was decoded — the media plane did not come up")
            ok = False
        elif rendered <= 0:
            print("FAIL: frames were decoded but none were rendered")
            ok = False
        else:
            print("OK: the media plane delivered and rendered frames")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
