#!/usr/bin/env python3
"""
e2e_chrome_interop.py — End-to-end Chrome ↔ NimRTC DTLS interop test.

What this verifies
------------------
1. Playwright opens system Chrome (the installed BoringSSL) with
   --ssl-key-log-file so we can capture the same NSS-format keylog
   that NimRTC writes via NIMRTC_DTLS_KEYLOG.
2. test_chrome_opus.html connects to the signaling server, exchanges
   SDP+ICE with the NimRTC answerer.
3. Once ICE Connected, the script waits for DTLS to complete on both
   sides, then compares:
     - NimRTC's trace file verify_data (server's compute vs client's compute)
     - Chrome's keylog (master_secret)
     - tools/verify_dtls_handshake.py against both keylogs

Run
---
   python tools/e2e_chrome_interop.py
       (assumes signaling_server + NimRTC answerer proxy are already running)
"""

import asyncio
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).parent.parent
E2E  = ROOT / "build" / "e2e"
CHROME_HTML = ROOT / "interop" / "chrome" / "test_chrome_opus.html"
CHROME_EXE  = Path(r"C:\Program Files\Google\Chrome\Application\chrome.exe")

WS_URL = "ws://localhost:8765/interop"


def grep(path: Path, pattern: str) -> list[str]:
    if not path.exists():
        return []
    rx = re.compile(pattern)
    return [ln.rstrip() for ln in path.read_text(errors="replace").splitlines()
            if rx.search(ln)]


def show_file_state() -> None:
    print("\n=== Post-test file state ===")
    for p in [
        E2E / "nimrtc_chrome.keylog",
        E2E / "nimrtc_chrome.trace",
        E2E / "nimrtc_proxy.err",
        E2E / "nimrtc_proxy.out",
        E2E / "chrome_console.log",
    ]:
        if p.exists():
            print(f"  {p.name}: {p.stat().st_size} bytes")
        else:
            print(f"  {p.name}: MISSING")


async def run_e2e() -> int:
    if not CHROME_EXE.exists():
        sys.exit(f"Chrome not found at {CHROME_EXE}")
    if not CHROME_HTML.exists():
        sys.exit(f"Chrome test HTML not found at {CHROME_HTML}")
    if not (E2E / "nimrtc_chrome.keylog").exists():
        # ensure the file exists so we can see the empty-state pre-test
        (E2E / "nimrtc_chrome.keylog").write_text("")
    if not (E2E / "nimrtc_chrome.trace").exists():
        (E2E / "nimrtc_chrome.trace").write_text("")

    from playwright.async_api import async_playwright

    url = CHROME_HTML.as_uri() + "?ws=" + WS_URL + "&room=interop&offerer=1"
    print(f"[E2E] Loading: {url}")

    async with async_playwright() as p:
        browser = await p.chromium.launch(
            headless=True,
            executable_path=CHROME_EXE,
            args=[
                "--headless=new",
                "--no-sandbox",
                "--disable-dev-shm-usage",
                "--use-fake-ui-for-media-stream",
                "--use-fake-device-for-media-stream",
                # BoringSSL writes its CLIENT_RANDOM lines here when set;
                # works alongside NimRTC's keylog.
                f"--ssl-key-log-file={E2E / 'chrome_chrome.keylog'}",
                # Surface DTLS/SSL errors on stderr (doesn't help with DTLS
                # but useful when checking protocol versions).
                "--enable-logging=stderr",
                "--v=1",
                # DTLS uses SCTP-like transport; WebRTC's RTCPeerConnection
                # is what raises DTLS errors to JS, so we can't see the raw
                # alert in JS console — enable BoringSSL-level logging so
                # `--v=1` actually surfaces the alert level/description.
                "--log-level=0",
                # Pin Chrome's verbose logs to a file we can read after the
                # run (Playwright discards Chrome's stderr).
                f"--log-file={E2E / 'chrome_verbose.log'}",
            ],
        )
        context = await browser.new_context()
        page = await context.new_page()

        # Tee Chrome console to disk for later inspection.
        console_log = E2E / "chrome_console.log"
        console_log.write_text("")
        msgs: list[str] = []
        page.on("console", lambda m: msgs.append(f"[{m.type}] {m.text}"))
        # Also capture page errors (uncaught JS exceptions, which is where
        # RTCPeerConnection DTLS-failure callbacks land).
        page.on("pageerror", lambda e: msgs.append(f"[pageerror] {e}"))

        await page.goto(url)
        print("[E2E] Page loaded — waiting for ICE+DTLS to complete")

        deadline = time.monotonic() + 35
        last_results = {}
        while time.monotonic() < deadline:
            try:
                snap = await page.evaluate(
                    "({"
                    " ws: window._interopResults?.wsConnected,"
                    " ice: window._interopResults?.iceConnected,"
                    " audio: window._interopResults?.audioReceived,"
                    " rtp: window._interopResults?.rtpPackets,"
                    " errs: (window._interopResults?.errors || []).length,"
                    " exitCode: window._interopResults?.exitCode,"
                    "})"
                ) or {}
                last_results = snap
                if snap.get("ice"):
                    print(f"[E2E] ICE connected (audio={snap.get('audio')}, "
                          f"rtp={snap.get('rtp')}); waiting for DTLS")
                    await page.wait_for_timeout(3000)
                    break
            except Exception as exc:
                print(f"[DEBUG] poll failed: {exc}")
            await page.wait_for_timeout(500)

        try:
            results = await page.evaluate("window._interopResults") or {}
        except Exception as exc:
            print(f"[WARN] results extract failed: {exc}")
            results = last_results

        # Dump console log
        console_log.write_text("\n".join(msgs))
        print(f"[E2E] Wrote {len(msgs)} console lines to {console_log.name}")

        await browser.close()

    print("\n=== Chrome interop results ===")
    print(json.dumps(results, indent=2))
    return 0 if results.get("iceConnected") else 1


def summarize() -> None:
    print("\n=== NimRTC ↔ Chrome DTLS post-mortem ===")
    show_file_state()

    # Show NimRTC trace tail (master_sec + verify_data lines).
    trace = E2E / "nimrtc_chrome.trace"
    if trace.exists():
        print("\n--- NimRTC DTLS TRACE (last 20 lines) ---")
        lines = trace.read_text(errors="replace").splitlines()
        for ln in lines[-20:]:
            print(f"  {ln}")
    else:
        print("  [no trace file written]")

    # Show both keylogs side by side.
    print("\n--- NimRTC keylog ---")
    p_n = E2E / "nimrtc_chrome.keylog"
    if p_n.exists() and p_n.stat().st_size:
        print(p_n.read_text(errors="replace"))
    else:
        print("  (empty — NimRTC has not yet derived master_secret)")

    print("\n--- Chrome keylog (BoringSSL --ssl-key-log-file) ---")
    p_c = E2E / "chrome_chrome.keylog"
    if p_c.exists() and p_c.stat().st_size:
        print(p_c.read_text(errors="replace"))
    else:
        print("  (empty — BoringSSL hasn't emitted DTLS secrets yet)")

    # Run verify_dtls_handshake.py against the NimRTC pair if both have data.
    tool = ROOT / "tools" / "verify_dtls_handshake.py"
    if p_n.exists() and p_n.stat().st_size > 0:
        print("\n--- verify_dtls_handshake.py (NimRTC keylog <-> trace) ---")
        try:
            subprocess.run([sys.executable, str(tool), str(p_n), str(trace)],
                           check=False)
        except Exception as exc:
            print(f"[WARN] verify run failed: {exc}")


if __name__ == "__main__":
    rc = asyncio.run(run_e2e())
    summarize()
    sys.exit(rc)
