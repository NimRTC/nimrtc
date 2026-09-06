#!/usr/bin/env python3
"""
inject_offer_active.py — Connect to the signaling server as a fake Chrome
offerer, send a synthetic Opus SDP offer + ICE candidates, then perform
real ICE connectivity checks (STUN binding requests) against the NimRTC
answerer's UDP socket.  We also listen on our local UDP port and respond
to NimRTC's STUN binding requests so ICE actually reaches Connected.

Robustness: uses select() with timeout, handles WinError 10054 (WSAECONNRESET)
by recreating the socket, and logs every received packet for debugging.
"""
import argparse
import asyncio
import json
import logging
import secrets
import select
import socket
import struct
import sys
import websockets

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)-8s] %(name)s: %(message)s",
)
logger = logging.getLogger("inject")

# STUN message constants (RFC 5389)
STUN_MAGIC_COOKIE = 0x2112A442
STUN_BINDING_REQUEST = 0x0001
STUN_BINDING_SUCCESS = 0x0101
STUN_ATTR_USERNAME = 0x0006
STUN_ATTR_PRIORITY = 0x0024
STUN_ATTR_USE_CANDIDATE = 0x0025
STUN_ATTR_FINGERPRINT = 0x8028
STUN_ATTR_ICE_CONTROLLED = 0x8029
STUN_ATTR_ICE_CONTROLLING = 0x802A
STUN_ATTR_XOR_MAPPED_ADDRESS = 0x0020


def stun_binding_request(username: str, tie_breaker: int) -> bytes:
    txn_id = secrets.token_bytes(12)
    attrs = b""
    user_bytes = username.encode("utf-8")
    attrs += struct.pack("!HH", STUN_ATTR_USERNAME, len(user_bytes)) + user_bytes
    pad = (-len(user_bytes)) % 4
    attrs += b"\x00" * pad
    priority = 1862270975
    attrs += struct.pack("!HHI", STUN_ATTR_PRIORITY, 4, priority)
    attrs += struct.pack("!HHQ", STUN_ATTR_ICE_CONTROLLED, 8, tie_breaker)
    header = struct.pack("!HHI", STUN_BINDING_REQUEST, len(attrs), STUN_MAGIC_COOKIE)
    return header + txn_id + attrs


def parse_stun_request(data: bytes):
    if len(data) < 20:
        return None
    msg_type, msg_len, magic = struct.unpack("!HHI", data[:8])
    if magic != STUN_MAGIC_COOKIE or msg_type != STUN_BINDING_REQUEST:
        return None
    if msg_len > len(data) - 20:
        return None
    txn_id = data[8:20]
    pos = 20
    username = ""
    tie_breaker = 0
    use_candidate = False
    while pos + 4 <= 20 + msg_len:
        attr_type, attr_len = struct.unpack("!HH", data[pos:pos+4])
        pos += 4
        if attr_type == STUN_ATTR_USERNAME:
            username = data[pos:pos+attr_len].decode("utf-8", errors="replace")
        elif attr_type == STUN_ATTR_ICE_CONTROLLED:
            tie_breaker = struct.unpack("!Q", data[pos:pos+8])[0]
        elif attr_type == STUN_ATTR_ICE_CONTROLLING:
            tie_breaker = struct.unpack("!Q", data[pos:pos+8])[0]
        elif attr_type == STUN_ATTR_USE_CANDIDATE:
            use_candidate = True
        pos += attr_len
        pos += (-attr_len) % 4
    return txn_id, username, tie_breaker, use_candidate


def make_stun_success(txn_id: bytes, mapped_addr: tuple[str, int]) -> bytes:
    ip_bytes = socket.inet_aton(mapped_addr[0])
    xor_ip = bytes(b ^ ((STUN_MAGIC_COOKIE >> ((3 - i) * 8)) & 0xFF) for i, b in enumerate(ip_bytes))
    xor_port = mapped_addr[1] ^ (STUN_MAGIC_COOKIE >> 16)
    attr_value = struct.pack("!xBHI", 0, 1, xor_port) + xor_ip
    attr = struct.pack("!HH", STUN_ATTR_XOR_MAPPED_ADDRESS, len(attr_value)) + attr_value
    header = struct.pack("!HHI", STUN_BINDING_SUCCESS, len(attr), STUN_MAGIC_COOKIE)
    return header + txn_id + attr


def chrome_opus_offer(ufrag="ChromeUfrag",
                       pwd="ChromePassword123456789012345678",
                       host="127.0.0.1",
                       port=62000) -> str:
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
a=sendrecv
a=rtcp-mux
a=rtpmap:111 opus/48000/2
a=fmtp:111 minptime=10;useinbandfec=1;stereo=0;sprop-stereo=0
a=ssrc:1000 cname:chrome-cname
a=candidate:1 1 udp 2122317823 {host} {port} typ host
"""


def _disable_udp_connreset(sock: socket.socket) -> None:
    """Windows-only: tell Winsock not to return WSAECONNRESET on
    subsequent recv() calls.  This is the standard fix for Windows
    UDP sockets that get ICMP-unreachable back after a previous send."""
    if sys.platform != "win32":
        return
    import ctypes
    from ctypes import wintypes
    SIO_UDP_CONNRESET = 0x9800000C
    # lpdwBytesReturned must be a non-NULL pointer to a DWORD.
    bytes_returned = wintypes.DWORD(0)
    # lpOverlapped and lpCompletionRoutine must both be NULL for
    # a non-overlapped socket (per MSDN WSAIoctl remarks).
    ret = ctypes.windll.ws2_32.WSAIoctl(
        wintypes.HANDLE(sock.fileno()),
        wintypes.DWORD(SIO_UDP_CONNRESET),
        ctypes.c_void_p(0), wintypes.DWORD(0),       # inBuf=NULL, inBufLen=0
        ctypes.c_void_p(0), wintypes.DWORD(0),       # outBuf=NULL, outBufLen=0
        ctypes.byref(bytes_returned),                # lpcbBytesReturned
        ctypes.c_void_p(0),                          # lpOverlapped=NULL
        ctypes.c_void_p(0),                          # lpCompletionRoutine=NULL
    )
    if ret != 0:
        err = ctypes.get_last_error() or ctypes.GetLastError()
        logger.warning("WSAIoctl(SIO_UDP_CONNRESET) failed: err=%d", err)
    else:
        logger.info("SIO_UDP_CONNRESET enabled (recv() will ignore ICMP unreachable)")


def udp_loop(local_port, remote_host, remote_port, remote_ufrag, local_ufrag,
             stop_event: asyncio.Event, stats: dict):
    """Blocking UDP listener + pinger, run on a background thread.
    Uses SIO_UDP_CONNRESET to ignore Windows ICMP-unreachable errors,
    so we can keep receiving on the same port after NimRTC's first
    unreachable-triggering send."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    _disable_udp_connreset(sock)
    sock.bind(("127.0.0.1", local_port))
    sock.setblocking(False)
    username = f"{remote_ufrag}:{local_ufrag}"
    logger.info("UDP listener+pinger: 127.0.0.1:%d -> %s:%d username=%r",
                local_port, remote_host, remote_port, username)

    tie_breaker = 0xDEADBEEF12345678
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)

    import time
    last_send = 0.0
    sock_errors = 0
    try:
        while not stop_event.is_set():
            now = time.monotonic()
            # Send STUN request every 100 ms
            if now - last_send > 0.1:
                pkt = stun_binding_request(username, tie_breaker)
                try:
                    sock.sendto(pkt, (remote_host, remote_port))
                    stats["sent"] += 1
                except OSError as e:
                    stats["send_errors"] += 1
                    if stats["send_errors"] <= 3:
                        logger.warning("sendto failed: %s", e)
                last_send = now
            # Wait briefly for data
            r, _, _ = select.select([sock], [], [], 0.05)
            if sock in r:
                try:
                    data, addr = sock.recvfrom(4096)
                except ConnectionResetError as e:
                    sock_errors += 1
                    stats["recv_errors"] += 1
                    if sock_errors <= 3:
                        logger.warning("recvfrom WSAECONNRESET (ICMP unreachable), reopening socket: %s", e)
                    # Re-open the socket to clear the bad state
                    try:
                        sock.close()
                    except Exception:
                        pass
                    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                    sock.bind(("127.0.0.1", local_port))
                    sock.setblocking(False)
                    continue
                except OSError as e:
                    stats["recv_errors"] += 1
                    if stats["recv_errors"] <= 3:
                        logger.warning("recvfrom failed: %s", e)
                    continue
                stats["recv"] += 1
                if stats["recv"] <= 3:
                    logger.info("RX packet #%d (%d bytes) from %s:%d first4=%s",
                                 stats["recv"], len(data), addr[0], addr[1],
                                 data[:4].hex())
                parsed = parse_stun_request(data)
                if parsed:
                    txn_id, user, tb, uc = parsed
                    stats["stun_rx"] += 1
                    if stats["stun_rx"] <= 5 or stats["stun_rx"] % 10 == 0:
                        logger.info("RX STUN req #%d from %s:%d user=%r use_candidate=%s",
                                     stats["stun_rx"], addr[0], addr[1], user, uc)
                    resp = make_stun_success(txn_id, addr)
                    try:
                        sock.sendto(resp, addr)
                        stats["stun_tx_success"] += 1
                    except OSError as e:
                        logger.warning("sendto STUN success failed: %s", e)
                else:
                    if stats["recv"] <= 3:
                        logger.info("RX non-STUN: first4=%s", data[:4].hex())
            # Pump asyncio briefly
            try:
                loop.run_until_complete(asyncio.sleep(0))
            except Exception:
                pass
    finally:
        try:
            sock.close()
        except Exception:
            pass
        logger.info("UDP loop done: %s", stats)


async def udp_task(local_port, remote_host, remote_port,
                   remote_ufrag, local_ufrag, stop_event, stats):
    """Run blocking UDP loop on a thread."""
    return await asyncio.to_thread(
        udp_loop, local_port, remote_host, remote_port,
        remote_ufrag, local_ufrag, stop_event, stats)


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--signaling", default="ws://localhost:8765/interop")
    ap.add_argument("--local-host", default="127.0.0.1")
    ap.add_argument("--local-port", type=int, default=62000)
    ap.add_argument("--remote-host", default="127.0.0.1")
    ap.add_argument("--remote-port", type=int, required=True)
    ap.add_argument("--wait", type=float, default=30.0)
    args = ap.parse_args()

    local_ufrag = "ChromeUfrag"
    offer = chrome_opus_offer(host=args.local_host, port=args.local_port)
    candidates = [f"candidate:1 1 udp 2122317823 {args.local_host} {args.local_port} typ host"]

    stats = {"sent": 0, "stun_tx_success": 0, "stun_rx": 0,
             "recv": 0, "recv_errors": 0, "send_errors": 0}

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

        remote_ufrag = ""
        answer_seen = False
        candidate_count = 0
        stop_ping = asyncio.Event()
        ping_task = None
        end = asyncio.get_event_loop().time() + args.wait
        while asyncio.get_event_loop().time() < end:
            try:
                raw = await asyncio.wait_for(ws.recv(), timeout=1.0)
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
                answer_sdp = msg.get("sdp", "")
                import re
                m = re.search(r"a=ice-ufrag:(\S+)", answer_sdp)
                if m:
                    remote_ufrag = m.group(1)
                logger.info("Got SDP answer (ufrag=%s, len=%d)",
                             remote_ufrag, len(answer_sdp))
                if not ping_task and remote_ufrag:
                    ping_task = asyncio.create_task(
                        udp_task(args.local_port, args.remote_host, args.remote_port,
                                 remote_ufrag, local_ufrag, stop_ping, stats))
            elif t == "candidate":
                candidate_count += 1
                cand = msg.get("candidate", "")
                logger.info("Got candidate #%d: %s", candidate_count, cand[:80])
            else:
                logger.info("Got msg type=%s", t)
        if ping_task:
            stop_ping.set()
            try:
                await asyncio.wait_for(ping_task, timeout=3.0)
            except asyncio.TimeoutError:
                pass
        logger.info("Done: answer_seen=%s candidate_count=%d stats=%s",
                    answer_seen, candidate_count, stats)


if __name__ == "__main__":
    asyncio.run(main())
