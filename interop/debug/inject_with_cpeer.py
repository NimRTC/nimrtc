"""Minimal WebRTC peer driver.

Talks to the local NimRTC signaling server as the "Chrome" peer:
- Connects as the second peer (after NimRTC has registered).
- When NimRTC sends us an SDP answer, parses out its ufrag + ICE port,
  and launches build/minimal_webrtc_peer.exe (a C++ UDP peer that
  sends STUN binding requests and replies with Binding Success).

This lets us verify the ICE layer of NimRTC against a working
STUN responder without needing full Chrome.

Usage:
  python interop/debug/inject_with_cpeer.py \
      --signaling ws://localhost:8765/interop \
      --local-port 62000 \
      --duration 30
"""
import argparse
import asyncio
import json
import logging
import os
import random
import re
import socket
import subprocess
import sys
import time

import websockets

logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s %(levelname)-7s %(message)s",
                    datefmt="%H:%M:%S")
log = logging.getLogger("inject+c")


def rand_str(n: int) -> str:
    return ''.join(random.choice('abcdefghijklmnopqrstuvwxyz0123456789')
                   for _ in range(n))


def make_offer(ufrag: str, pwd: str, listen_port: int) -> str:
    """Build a minimal SDP offer matching NimRTC's expectations.

    We act as the controlling peer, send a single host candidate at
    127.0.0.1:listen_port.  Audio-only Opus + a single m=audio section.
    """
    fingerprint = ("7A:11:48:BC:6E:F4:80:9D:22:71:E1:DA:53:11:81:98:"
                    "F3:69:96:11:38:82:9B:0F:3C:65:DA:2F:4C:5E:FF:AB:"
                    "11:48:BC:6E:F4:80:9D:22:71:E1:DA:53:11:81:98:F3")
    setup = "actpass"
    candidates = (f"a=candidate:1 1 UDP 2122317823 127.0.0.1 {listen_port} "
                  f"typ host")
    return "\r\n".join([
        "v=0",
        "o=- 123456 2 IN IP4 127.0.0.1",
        "s=-",
        "t=0 0",
        "a=group:BUNDLE 0",
        "a=msid-semantic: WMS",
        "m=audio 9 UDP/TLS/RTP/SAVPF 111 63 9 0 8 13 110 126",
        "c=IN IP4 0.0.0.0",
        "a=rtcp:9 IN IP4 0.0.0.0",
        f"a=candidate:1 1 UDP 2122317823 127.0.0.1 {listen_port} typ host",
        f"a=ice-ufrag:{ufrag}",
        f"a=ice-pwd:{pwd}",
        f"a=fingerprint:sha-256 {fingerprint}",
        "a=setup:actpass",
        "a=mid:0",
        "a=sendrecv",
        "a=rtcp-mux",
        "a=rtpmap:111 opus/48000/2",
        "a=rtcp-fb:111 nack",
        "a=fmtp:111 minptime=10;useinbandfec=1",
        "a=rtpmap:63 red/48000/2",
        "a=fmtp:63 111/111",
        "a=rtpmap:9 G722/8000",
        "a=rtpmap:0 PCMU/8000",
        "a=rtpmap:8 PCMA/8000",
        "a=rtpmap:13 CN/8000",
        "a=rtpmap:110 telephone-event/48000",
        "a=rtpmap:126 telephone-event/8000",
        "",
    ])


def parse_answer(sdp: str):
    """Extract NimRTC's ufrag, fingerprint, and host candidate."""
    ufrag = pwd = fingerprint = cand = None
    for line in sdp.splitlines():
        line = line.strip()
        if line.startswith("a=ice-ufrag:"):
            ufrag = line.split(":", 1)[1]
        elif line.startswith("a=ice-pwd:"):
            pwd = line.split(":", 1)[1]
        elif line.startswith("a=fingerprint:"):
            fingerprint = line.split(":", 2)[2]
        elif line.startswith("a=candidate:") and "127.0.0.1" in line and "host" in line:
            cand = line
    return ufrag, pwd, fingerprint, cand


def parse_candidate(cand_line: str):
    m = re.match(
        r"a=candidate:\d+ \d+ UDP (\d+) (\S+) (\d+) typ (\S+)", cand_line)
    if not m:
        return None
    return {
        "priority": int(m.group(1)),
        "ip": m.group(2),
        "port": int(m.group(3)),
        "type": m.group(4),
    }


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--signaling", default="ws://localhost:8765/interop")
    ap.add_argument("--local-port", type=int, default=62000,
                    help="port we (Chrome-side) will listen on for STUN")
    ap.add_argument("--duration", type=int, default=30)
    ap.add_argument("--peer-binary",
                    default=os.path.join(os.path.dirname(__file__),
                                         "..", "..", "build",
                                         "minimal_webrtc_peer.exe"))
    ap.add_argument("--wait", type=int, default=20,
                    help="max seconds to wait for SDP answer from NimRTC")
    args = ap.parse_args()

    peer_binary = os.path.abspath(args.peer_binary)
    if not os.path.exists(peer_binary):
        log.error("peer binary not found: %s — build it first with cl", peer_binary)
        sys.exit(1)

    log.info("connecting to %s", args.signaling)
    async with websockets.connect(args.signaling,
                                  ping_interval=10) as ws:
        # NimRTC's signaling_proxy treats every line as JSON {type,...}.
        # An unknown type like "register" causes demo-p2p to print
        # "malformed JSON" / move to closed (we observed state=closed
        # right after a stray register).  So we just wait for NimRTC
        # to publish itself (the existing protocol buffers messages
        # for late joiners) and then send our offer.

        ufrag = rand_str(4)
        pwd = rand_str(24)
        offer = make_offer(ufrag, pwd, args.local_port)
        log.info("sending offer ufrag=%s len=%d", ufrag, len(offer))
        await ws.send(json.dumps({"type": "offer", "sdp": offer}))

        # Wait for answer
        answer_sdp = None
        deadline = time.time() + args.wait
        while time.time() < deadline:
            try:
                msg = await asyncio.wait_for(ws.recv(),
                                              timeout=deadline - time.time())
            except (asyncio.TimeoutError, TimeoutError):
                break
            data = json.loads(msg)
            log.info("got msg: %s", list(data.keys()))
            if data.get("type") == "answer":
                answer_sdp = data.get("sdp", "")
                break
            elif data.get("type") == "candidate":
                pass  # we ignore trickle for now

        if not answer_sdp:
            log.error("no SDP answer received within %ds", args.wait)
            return

        remote_ufrag, _, _, cand_line = parse_answer(answer_sdp)
        if not (remote_ufrag and cand_line):
            log.error("could not parse answer: ufrag=%r cand=%r",
                      remote_ufrag, cand_line)
            return
        c = parse_candidate(cand_line)
        if not c:
            log.error("could not parse candidate: %r", cand_line)
            return
        log.info("answer: remote_ufrag=%s remote_host=%s:%d",
                 remote_ufrag, c["ip"], c["port"])

        # Launch the C++ peer to drive STUN
        peer_args = [
            peer_binary,
            "--listen", f"127.0.0.1:{args.local_port}",
            "--remote", f"{c['ip']}:{c['port']}",
            "--remote-ufrag", remote_ufrag,
            "--local-ufrag", ufrag,
            "--duration", str(args.duration),
        ]
        log.info("launching C++ peer: %s", " ".join(peer_args))
        log_path = os.path.abspath(os.path.join(os.path.dirname(peer_binary),
                                                "cpeer.log"))
        with open(log_path, "w", encoding="utf-8") as logf:
            proc = subprocess.Popen(peer_args, stdout=logf, stderr=subprocess.STDOUT)
        log.info("cpeer pid=%d, log=%s — waiting %ds",
                 proc.pid, log_path, args.duration + 5)

        # Wait for either the cpeer to finish or the duration
        try:
            rc = await asyncio.to_thread(proc.wait,
                                         timeout=args.duration + 10)
            log.info("cpeer exited rc=%d, log:", rc)
            with open(log_path, "r", encoding="utf-8", errors="replace") as f:
                sys.stdout.write(f.read())
        except subprocess.TimeoutExpired:
            log.warning("cpeer still running after %ds, killing",
                        args.duration + 10)
            proc.kill()


if __name__ == "__main__":
    asyncio.run(main())
