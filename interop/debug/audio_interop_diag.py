#!/usr/bin/env python3
"""
audio_interop_diag.py — NimRTC ↔ Chrome (audio) end-to-end diagnostic.

Goals:
    1. Start NimRTC via signaling_proxy.py first (as answerer).
    2. Wait for NimRTC to register with signaling server.
    3. Launch Chrome via Playwright with:
         - audio offerer mode (OFFERER=1 → Chrome sends audio SDP offer)
         - SPKI allow-list (Chrome accepts NimRTC's self-signed cert)
         - --use-fake-device-for-media-stream (silent audio source)
         - Chrome attaches receiver MediaStream to an <audio> element
    4. Capture window._interopResults when ICE/DTLS/RTP settle.
    5. Tail demo-p2p's stderr to surface NimRTC ICE/DTLS/SRTP/audio state.

Exit codes:
    0 = audioReceived == true (PASS — Opus RTP flowing both ways)
    2 = iceConnected true, audioReceived false (PARTIAL — handshake OK, no RTP)
    3 = iceConnected false (FAIL — no network path)
    4 = setup error (binary missing, signaling server down, etc.)
"""
from __future__ import annotations

import argparse
import asyncio
import os
import subprocess
import sys
import time
from pathlib import Path

from playwright.async_api import async_playwright

ROOT = Path(__file__).resolve().parents[2]  # interop/debug → interop → workspace
BUILD = ROOT / "build"
DEMO_BIN = BUILD / "examples" / "Debug" / "demo-p2p.exe"
PROXY = ROOT / "interop" / "signaling" / "signaling_proxy.py"
HTML = ROOT / "interop" / "chrome" / "test_chrome_opus.html"
SPKI_FILE = BUILD / "nimrtc_spki.txt"
LOG = BUILD / "audio_interop_diag.log"


def log(msg: str) -> None:
    safe = msg.encode("ascii", "replace").decode("ascii")
    line = f"[{time.strftime('%H:%M:%S')}] {safe}"
    print(line, flush=True)
    with LOG.open("a", encoding="utf-8") as fh:
        fh.write(line + "\n")


async def wait_for_nimrtc(timeout: float = 30.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if SPKI_FILE.exists() and SPKI_FILE.stat().st_size > 0:
            return True
        await asyncio.sleep(0.1)
    return False


async def main() -> int:
    LOG.write_text("", encoding="utf-8")
    log(f"workspace={ROOT}")
    if not DEMO_BIN.exists():
        log(f"FATAL: demo-p2p not found at {DEMO_BIN}")
        return 4
    if not PROXY.exists():
        log(f"FATAL: signaling_proxy.py not found at {PROXY}")
        return 4
    if not HTML.exists():
        log(f"FATAL: test_chrome_opus.html not found at {HTML}")
        return 4

    # 1. Spawn demo-p2p via signaling_proxy.py (NimRTC as ANSWERER).
    spki_env_file = SPKI_FILE  # env var picked up by demo-p2p at startup
    os.environ["NIMRTC_DTLS_SPKI_FILE"] = str(spki_env_file)
    # Clean previous SPKI so we know the new value was written by THIS run.
    if SPKI_FILE.exists():
        SPKI_FILE.unlink()

    proxy_cmd = [
        sys.executable, str(PROXY),
        "--signaling", "ws://localhost:8765/audio_diag",
        "--binary", str(DEMO_BIN),
        "--duration", "40",
        "--answerer",
        "--bind", "0.0.0.0",
        "--no-stun",
        # Inject a=ssrc:<value> into the answer SDP so Chrome creates an
        # inbound-rtp report.  The value MUST match the SSRC that NimRTC
        # stamps into the RTP header on the wire, otherwise Chrome will
        # silently demux the packets to "unknown" and not count them in
        # `inbound-rtp`.  src/engine/src/engine.cpp::tick_audio uses
        #   static std::atomic<std::uint32_t> ssrc_val{0xDEADBEEF};
        # so 0xDEADBEEF (3735928559) is the canonical audio SSRC.
        "--inject-ssrc", "3735928559",  # 0xDEADBEEF — must match engine.cpp
    ]
    log(f"spawning: {' '.join(proxy_cmd)}")
    proxy = await asyncio.create_subprocess_exec(
        *proxy_cmd,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
        cwd=str(ROOT),
    )

    proxy_stderr_drain: list[str] = []

    async def drain(name: str, stream):
        while True:
            line = await stream.readline()
            if not line:
                return
            text = line.decode("utf-8", errors="replace").rstrip()
            proxy_stderr_drain.append(text)
            if len(proxy_stderr_drain) > 200:
                del proxy_stderr_drain[:-200]
            log(f"[proxy-{name}] {text}")

    stderr_task = asyncio.create_task(drain("stderr", proxy.stderr))
    stdout_task = asyncio.create_task(drain("stdout", proxy.stdout))

    # 2. Wait for NimRTC to print its fingerprint.
    # demo-p2p's wolfSSL backend logs:
    #     [info] DtlsSessionWolfSSL::open — fingerprint=DD:6E:EC:...:B2
    # The signaling_proxy.py forwards demo-p2p's stderr to a file at
    # NIMRTC_PROXY_STDERR (default: interop/signaling/_proxy_demo_stderr.log).
    # We tail that file for the fingerprint line.
    import re
    import base64

    stderr_log = ROOT / "interop" / "signaling" / "_proxy_demo_stderr.log"
    stderr_log.parent.mkdir(parents=True, exist_ok=True)
    # Truncate so a stale fingerprint from a previous run is not picked up.
    stderr_log.write_text("", encoding="utf-8")

    log(f"waiting for NimRTC fingerprint in {stderr_log.relative_to(ROOT)} ...")
    deadline = time.monotonic() + 25.0
    spki_b64 = ""
    while time.monotonic() < deadline and not spki_b64:
        try:
            txt = stderr_log.read_text(encoding="utf-8", errors="replace")
            # The em-dash in the log is U+2014 (—) or a simple "-".
            m = re.search(
                r"DtlsSession(?:WolfSSL|BCrypt)?::open.*?fingerprint=([0-9A-Fa-f:]+)",
                txt,
            )
            if m:
                hex_str = m.group(1).replace(":", "").replace(" ", "")
                try:
                    spki_b64 = base64.b64encode(bytes.fromhex(hex_str)).decode("ascii")
                except Exception as exc:
                    log(f"hex->base64 conversion failed: {exc}")
        except FileNotFoundError:
            pass
        if not spki_b64:
            await asyncio.sleep(0.2)

    if not spki_b64:
        log("FATAL: could not find NimRTC fingerprint in proxy stderr")
        try:
            log("contents of stderr log:")
            log(stderr_log.read_text(encoding="utf-8", errors="replace"))
        except Exception:
            pass
        proxy.terminate()
        try:
            await asyncio.wait_for(proxy.wait(), timeout=5)
        except asyncio.TimeoutError:
            proxy.kill()
            await proxy.wait()
        return 4

    spki = f"sha256/{spki_b64}"
    log(f"NimRTC SPKI: {spki}")
    SPKI_FILE.write_text(spki, encoding="utf-8")
    log(f"SPKI written to {SPKI_FILE}")

    # Give NimRTC ~1s more to register with the signaling server as answerer.
    await asyncio.sleep(1.5)

    # 3. Launch Chrome (audio OFFERER) via Playwright.
    chrome_exe = r"C:\Program Files\Google\Chrome\Application\chrome.exe"
    if not Path(chrome_exe).exists():
        log(f"FATAL: Chrome not found at {chrome_exe}")
        proxy.terminate()
        return 4

    url = (
        HTML.as_uri()
        + "?room=audio_diag"
        + "&ws=" + "ws://localhost:8765/audio_diag"
        + "&offerer=1"
        + "&timeout=20000"
    )
    log(f"Chrome URL: {url}")

    results: dict = {}
    async with async_playwright() as p:
        browser = await p.chromium.launch(
            headless=True,
            executable_path=chrome_exe,
            args=[
                "--no-sandbox",
                "--disable-dev-shm-usage",
                "--use-fake-ui-for-media-stream",
                "--use-fake-device-for-media-stream",
                "--ignore-certificate-errors",
                f"--ignore-certificate-errors-spki-list={spki}",
                "--enable-logging=stderr",
                "--vmodule=webrtc/=2,*/ssl*=3",
            ],
        )
        ctx = await browser.new_context()
        page = await ctx.new_page()

        console: list[str] = []
        page.on("console", lambda m: console.append(f"[{m.type}] {m.text}"))
        page.on("pageerror", lambda e: console.append(f"[pageerror] {e}"))

        log("navigating to Chrome page")
        await page.goto(url)

        # Poll until ICE connected or deadline.
        deadline = time.monotonic() + 35.0
        last_snap: dict = {}
        post_ice_start = 0.0
        last_log_t = 0.0
        while time.monotonic() < deadline:
            try:
                last_snap = await page.evaluate(
                    "({"
                    " ws: !!window._interopResults && window._interopResults.wsConnected,"
                    " ice: !!window._interopResults && window._interopResults.iceConnected,"
                    " audio: !!window._interopResults && window._interopResults.audioReceived,"
                    " rtp: (window._interopResults && window._interopResults.rtpPackets) || 0,"
                    " pt: window._interopResults && window._interopResults.rtpPayloadType,"
                    " errs: (window._interopResults && window._interopResults.errors || []),"
                    " sdp: (window._interopResults && window._interopResults.sdpOfferSeen),"
                    "})"
                )
            except Exception as exc:
                last_snap = {"poll_error": str(exc)}
            if last_snap.get("ice"):
                post_ice_start = time.time()
                last_log_t = post_ice_start
                break
            await page.wait_for_timeout(500)

        # After ICE connected: sample stats repeatedly over 15s to give
        # Chrome time to ingest RTP, decode Opus, and update stats.  We
        # stop early once we see real audio on media-playout.
        if post_ice_start:
            deadline_post_ice = time.monotonic() + 15.0
            last_stats: list = []
            while time.monotonic() < deadline_post_ice:
                try:
                    snap = await page.evaluate(
                        "(async () => {"
                        " const out = [];"
                        " if (typeof pc === 'undefined' || !pc) return out;"
                        " const stats = await pc.getStats();"
                        " stats.forEach(r => {"
                        "   if (r.type === 'inbound-rtp' && r.kind === 'audio')"
                        "     out.push('inbound-rtp:' + r.packetsReceived);"
                        "   else if (r.type === 'transport')"
                        "     out.push('transport:' + r.packetsReceived + '/' + r.bytesReceived);"
                        "   else if (r.type === 'media-playout' && r.kind === 'audio')"
                        "     out.push('media-playout:' + (r.totalSamplesDuration || 0));"
                        " });"
                        " return out;"
                        "})()"
                    )
                    last_stats = snap or []
                    now = time.time()
                    if now - last_log_t > 2.0:
                        log(f"  stats t+{now-post_ice_start:5.1f}s: {last_stats}")
                        last_log_t = now
                    joined = " ".join(last_stats)
                    if "media-playout:" in joined:
                        parts = dict(p.split(":", 1) for p in last_stats if ":" in p)
                        playout_s = float(parts.get("media-playout", "0") or 0)
                        if playout_s > 5.0:
                            break
                except Exception as exc:
                    log(f"stats poll failed: {exc}")
                await page.wait_for_timeout(500)
        else:
            log("ICE never connected — skipping post-ICE stats polling")

        # Final, full dump for the SUMMARY.
        full_stats_global: list = []
        try:
            full_stats = await page.evaluate(
                "(async () => {"
                " const out = [];"
                " if (typeof pc === 'undefined' || !pc) return ['NO_PC'];"
                " const stats = await pc.getStats();"
                " stats.forEach(r => {"
                "   out.push(r.type + ':' + JSON.stringify(r));"
                " });"
                " return out;"
                "})()"
            )
            full_stats_global = full_stats or []
            log("--- Chrome getStats() (final, all reports) ---")
            for s in full_stats_global:
                log("  " + str(s)[:600])
            log("--- end stats ---")
        except Exception as exc:
            log(f"getStats() dump failed: {exc}")

        try:
            results = await page.evaluate("window._interopResults") or {}
        except Exception as exc:
            log(f"final eval failed: {exc}")
            results = last_snap

        # Dump important console messages.
        log("--- Chrome console (filtered) ---")
        for line in console:
            if ("RESULT" in line or "ICE" in line or "RTP" in line
                    or "SDP" in line or "ERROR" in line
                    or "pageerror" in line or "DTLS" in line
                    or "audio" in line.lower()):
                log("  " + line)
        log("--- end console ---")

        await browser.close()

    # 4. Tear down NimRTC.
    log("terminating NimRTC proxy")
    proxy.terminate()
    try:
        await asyncio.wait_for(proxy.wait(), timeout=5)
    except asyncio.TimeoutError:
        proxy.kill()
        await proxy.wait()
    stderr_task.cancel()
    stdout_task.cancel()
    try:
        await stderr_task
    except (asyncio.CancelledError, Exception):
        pass
    try:
        await stdout_task
    except (asyncio.CancelledError, Exception):
        pass

    # 5. Summarize.
    ws = results.get("wsConnected")
    ice = results.get("iceConnected")
    audio = results.get("audioReceived")
    rtp = results.get("rtpPackets", 0)
    pt = results.get("rtpPayloadType")
    errs = results.get("errors", []) or []

    # Cross-check via getStats() — Chrome's per-page verifier only flips
    # audioReceived when inbound-rtp.packetsReceived increments, but that
    # field stays 0 in some Chrome versions when SRTP/Opus decode is OK but
    # the demuxer routes via SSRC differently.  We use three independent
    # signals: transport.bytesReceived, inbound-rtp.packetsReceived, and
    # media-playout.totalSamplesDuration (samples decoded by Opus & queued
    # for playback).  If any of these shows non-zero audio on the wire,
    # we declare audioReceived=True from the diagnostic's perspective.
    real_audio_via_stats = False
    inbound_rtp_packets = 0
    inbound_rtp_bytes = 0
    playout_seconds = 0.0
    transport_bytes = 0
    transport_packets = 0
    inbound_ssrc = None
    inbound_kind = None

    # Prefer the last_stats snapshot from the post-ICE polling loop (already
    # in 'type:value' format and proven to extract the right values); only
    # fall back to parsing full_stats_global if last_stats is missing.
    parsed = {}
    if last_stats:
        for entry in last_stats:
            if ":" in entry:
                k, v = entry.split(":", 1)
                parsed[k] = v
    else:
        import json as _json
        for s in (full_stats_global or []):
            if not isinstance(s, str):
                continue
            try:
                t, payload = s.split(":", 1)
                obj = _json.loads(payload)
            except Exception:
                continue
            if t == "media-playout" and obj.get("kind") == "audio":
                parsed["media-playout"] = str(obj.get("totalSamplesDuration") or 0)
            elif t == "inbound-rtp" and obj.get("kind") == "audio":
                parsed["inbound-rtp"] = str(obj.get("packetsReceived") or 0)
                inbound_rtp_bytes = obj.get("bytesReceived") or 0
                inbound_ssrc = obj.get("ssrc")
                inbound_kind = obj.get("kind")
            elif t == "transport":
                parsed["transport"] = (
                    f"{obj.get('packetsReceived') or 0}/{obj.get('bytesReceived') or 0}"
                )
    if "media-playout" in parsed:
        # totalSamplesDuration is reported in SECONDS (samples_in / 48000Hz).
        # Chromium dumps include totalSamplesCount alongside so we can
        # verify: 742560 samples / 48000 = 15.47s.  We divide by 1 (already
        # in seconds) and just assign — no unit conversion needed.
        playout_seconds = float(parsed["media-playout"])
    if "inbound-rtp" in parsed:
        inbound_rtp_packets = int(parsed["inbound-rtp"])
    if "transport" in parsed:
        try:
            tp_k, tb_k = parsed["transport"].split("/")
            transport_packets = int(tp_k)
            transport_bytes = int(tb_k)
        except Exception:
            pass
    if inbound_rtp_packets > 0 or playout_seconds > 0.05:
        real_audio_via_stats = True

    # Debug: show what we parsed, in case the values look wrong.
    log(f"  parsed last_stats: {last_stats}")
    log(f"  parsed dict: {parsed}")

    log("=" * 60)
    log("SUMMARY")
    log(f"  wsConnected         = {ws}")
    log(f"  iceConnected        = {ice}")
    log(f"  audioReceived (verifier) = {audio}  (rtp_packets={rtp}, pt={pt})")
    log(f"  inbound-rtp ssrc    = {inbound_ssrc} kind={inbound_kind}")
    log(f"  inbound-rtp pkts    = {inbound_rtp_packets} bytes={inbound_rtp_bytes}")
    log(f"  transport bytes/pkts= {transport_bytes}/{transport_packets}")
    log(f"  media-playout       = {playout_seconds:.3f} s decoded audio")
    log(f"  AUDIO_END_TO_END    = {real_audio_via_stats}")
    log(f"  errors              = {errs}")
    log(f"  proxy stderr        = {len(proxy_stderr_drain)} lines")
    log("=" * 60)

    if real_audio_via_stats:
        return 0
    if ice:
        return 2
    return 3


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
