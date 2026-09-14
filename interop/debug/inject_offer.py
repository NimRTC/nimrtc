#!/usr/bin/env python3
"""
inject_offer.py — Connect to the signaling server as a fake Chrome
offerer, send a synthetic Opus SDP offer + ICE candidates, and wait for
the NimRTC answerer to respond.  Used to debug the NimRTC answerer
path without spinning up Playwright/Chrome.
"""
import argparse
import asyncio
import json
import logging
import os
import urllib.parse
import websockets
from pathlib import Path

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)-8s] %(name)s: %(message)s",
)
logger = logging.getLogger("inject")


def chrome_opus_offer(ufrag: str = "ChromeUfrag",
                       pwd: str = "ChromePassword123456789012345678",
                       host: str = "127.0.0.1",
                       port: int = 51000) -> str:
    """Minimal Chrome-shaped Opus SDP offer."""
    return f"""v=0
o=- 1234567890 1234567890 IN IP4 0.0.0.0
s=-
t=0 0
a=group:BUNDLE 0
a=msid-semantic: WMS *
m=audio 9 UDP/TLS/RTP/SAVPF 111
c=IN IP4 0.0.0.0
a=rtcp:9 IN IP4 0.0.0.0
a=ice-ufrag:{ufrag}
a=ice-pwd:{pwd}
a=ice-options:trickle
a=fingerprint:sha-256 AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66
a=setup:actpass
a=mid:0
a=extmap:1 urn:ietf:params:rtp-hdrext:ssrc-audio-level
a=extmap:4 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01
a=extmap:2 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time
a=sendrecv
a=rtcp-mux
a=rtcp-rsize
a=rtpmap:111 opus/48000/2
a=fmtp:111 minptime=10;useinbandfec=1;stereo=0;sprop-stereo=0
a=ssrc:1000 cname:chrome-cname
a=ssrc:1000 msid:chrome-audio audio-track
a=ssrc:1000 mslabel:chrome-audio
a=ssrc:1000 label:audio-track
a=candidate:1 1 udp 2122317823 {host} {port} typ host
"""


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--signaling", default="ws://localhost:8765/interop")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=51000)
    ap.add_argument("--wait", type=float, default=20.0,
                    help="seconds to wait for ICE/DTLS to settle")
    args = ap.parse_args()

    offer = chrome_opus_offer(host=args.host, port=args.port)
    candidates = [f"candidate:1 1 udp 2122317823 {args.host} {args.port} typ host"]

    logger.info("Connecting to %s", args.signaling)
    async with websockets.connect(args.signaling) as ws:
        logger.info("WS open, sending offer")
        await ws.send(json.dumps({"type": "offer", "sdp": offer}))
        for c in candidates:
            await ws.send(json.dumps({
                "type": "candidate",
                "candidate": c,
                "sdpMLineIndex": 0,
                "sdpMid": "0",
            }))
        # Wait for the answerer to reply.
        answer_seen = False
        candidate_count = 0
        end = asyncio.get_event_loop().time() + args.wait
        while asyncio.get_event_loop().time() < end:
            try:
                raw = await asyncio.wait_for(ws.recv(), timeout=2.0)
            except asyncio.TimeoutError:
                continue
            except websockets.exceptions.ConnectionClosed:
                logger.info("WS closed by peer")
                break
            msg = json.loads(raw)
            t = msg.get("type")
            sender = msg.get("from", "?")
            if t == "answer":
                answer_seen = True
                logger.info("Got SDP answer from %s (len=%d)",
                             sender[:8], len(msg.get("sdp", "")))
            elif t == "candidate":
                candidate_count += 1
                cand = msg.get("candidate", "")
                logger.info("Got candidate #%d from %s: %s",
                             candidate_count, sender[:8], cand[:80])
            else:
                logger.info("Got msg type=%s", t)
        logger.info("Done: answer_seen=%s candidate_count=%d",
                    answer_seen, candidate_count)


if __name__ == "__main__":
    asyncio.run(main())
