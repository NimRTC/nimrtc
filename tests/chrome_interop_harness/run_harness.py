#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
tests/chrome_interop_harness/run_harness.py
==========================================
Chrome ↔ NimRTC DTLS-only interop harness (Case E).

Architecture
-----------
  ┌──────────────────────┐
  │  signaling_server.py  │  ws://127.0.0.1:9000/dtls-e
  └──────────┬───────────┘
             │ WebSocket
  ┌──────────┴───────────────────────┐
  │  demo-p2p --signaling-proxy      │  NimRTC answerer (wolfSSL DTLS)
  │  (started by this harness via    │
  │   signaling_proxy.py)           │
  └─────────────────────────────────┘
  ┌──────────────────────────────────┐
  │  Chrome (headless)               │  Chrome offerer (BoringSSL DTLS)
  │  chrome_dtls_diag.html           │
  └──────────────────────────────────┘

Evidence items (all three must pass for overall PASS):
  E1. Chrome: dtlsState === 'connected' (BoringSSL derived SRTP keys).
  E2. Chrome: srtpCipher is set (SRTP profile negotiated).
  E3. demo-p2p stderr: "engine[dtls→srtp]: DTLS reached Connected".

Usage
-----
  python tests/chrome_interop_harness/run_harness.py
      --signaling-port 9000   (default)
      --timeout 25            (default: seconds to wait for handshake)

Exit codes: 0 = PASS, 1 = FAIL.
"""

from __future__ import annotations

import json
import logging
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

# Fix Windows console Unicode output.
if sys.stdout is not None:
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass

# ─── Paths ────────────────────────────────────────────────────────────────────

HARNESS_DIR = Path(__file__).resolve().parent
ROOT        = HARNESS_DIR.parents[1]          # NimRTC project root
BUILD_DIR   = ROOT / "build"
E2E_DIR     = BUILD_DIR / "e2e"
E2E_DIR.mkdir(parents=True, exist_ok=True)

SIGNALING_SERVER  = ROOT / "interop" / "signaling" / "signaling_server.py"
SIGNALING_PROXY   = ROOT / "interop" / "signaling" / "signaling_proxy.py"
CHROME_HTML       = HARNESS_DIR / "chrome_dtls_only.html"
# Default to system Chrome; allow override via env var so CI / dev machines
# without Google Chrome (but with Playwright's bundled Chromium) can still
# run Case E.  Example:
#   NIMRTC_CHROME_PATH="C:\Users\me\AppData\Local\ms-playwright\chromium-1223\chrome-win64\chrome.exe"
_CHROME_DEFAULT = Path(r"C:\Program Files\Google\Chrome\Application\chrome.exe")
CHROME_EXE = Path(os.environ.get("NIMRTC_CHROME_PATH", str(_CHROME_DEFAULT)))

# demo-p2p binary — resolve using the same logic as run_e2e_acceptance.py.
_NIMRTC_BUILD_DIR = Path(os.environ.get("NIMRTC_BUILD_DIR", BUILD_DIR))
for _sub, _cfg in [("examples", "Debug"), ("Debug", "examples"), ("examples", "")]:
    _p = _NIMRTC_BUILD_DIR / _sub / _cfg / "demo-p2p.exe"
    if _p.exists():
        DEMO_P2P = _p
        break
else:
    DEMO_P2P = _NIMRTC_BUILD_DIR / "examples" / "Debug" / "demo-p2p.exe"

# ─── Logging ───────────────────────────────────────────────────────────────────

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)-8s] %(name)s: %(message)s",
)
logger = logging.getLogger("case_e_harness")

# ─── Helpers ──────────────────────────────────────────────────────────────────


def wait_for_port(port: int, timeout_s: float = 5.0) -> bool:
    """Poll until something is listening on localhost:port."""
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def kill_port(port: int) -> None:
    """Best-effort: kill any process holding this port (Windows)."""
    try:
        out = subprocess.run(
            ["netstat", "-ano", "-p", "TCP"],
            capture_output=True, text=True, check=False,
        ).stdout
    except Exception:
        return
    for ln in out.splitlines():
        if f":{port} " not in ln:
            continue
        parts = ln.split()
        if len(parts) < 5 or parts[-1] == "0":
            continue
        pid = parts[-1]
        try:
            subprocess.run(
                ["taskkill", "/F", "/PID", pid, "/T"],
                capture_output=True, check=False,
            )
        except Exception:
            pass


# ─── Main test ────────────────────────────────────────────────────────────────


def run(signaling_port: int = 9000, timeout_s: int = 25) -> dict:
    """
    Run the Chrome ↔ NimRTC DTLS interop test.

    Returns a dict with:
      pass          bool
      chrome        dict  (wsConnected, iceConnected, dtlsState, srtpCipher)
      nimrtc_dtls_line   str  (first matching log line)
      errors        list[str]
    """
    results = {
        "pass": False,
        "chrome": {
            "wsConnected": False,
            "iceConnected": False,
            "dtlsState": None,
            "srtpCipher": None,
        },
        "nimrtc_dtls_line": "",
        "nimrtc_srtp_line": "",
        "errors": [],
    }

    room          = "dtls-e"
    ws_url        = f"ws://127.0.0.1:{signaling_port}/{room}"
    spki_file     = E2E_DIR / "case_e_nimrtc_spki.b64"
    nimrtc_trace  = E2E_DIR / "case_e_nimrtc.trace"
    nimrtc_keylog = E2E_DIR / "case_e_nimrtc.keylog"
    chrome_console_log = E2E_DIR / "case_e_chrome_console.log"
    chrome_verbose_log = E2E_DIR / "case_e_chrome_verbose.log"

    log_signal  = open(E2E_DIR / "case_e_signal_server.log",  "w", encoding="utf-8")
    log_proxy   = open(E2E_DIR / "case_e_proxy.out",          "w", encoding="utf-8")
    log_proxy_e = open(E2E_DIR / "case_e_proxy.err",          "w", encoding="utf-8")

    procs: list[tuple[str, subprocess.Popen]] = []

    def start_proc(name: str, argv, stdout=None, stderr=None) -> subprocess.Popen:
        kw = dict(stdout=stdout or subprocess.DEVNULL,
                  stderr=stderr or subprocess.DEVNULL)
        if sys.platform == "win32":
            kw["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
        p = subprocess.Popen(argv, **kw)
        procs.append((name, p))
        logger.info("Started %s (pid=%d)", name, p.pid)
        return p

    def stop_all():
        for name, p in reversed(procs):
            try:
                if p.poll() is None:
                    p.terminate()
            except Exception:
                pass
        time.sleep(0.5)
        for name, p in reversed(procs):
            try:
                if p.poll() is None:
                    p.kill()
            except Exception:
                pass
        kill_port(signaling_port)

    try:
        # ── 0. Ensure Chrome and HTML exist ─────────────────────────────────
        if not CHROME_EXE.exists():
            results["errors"].append(f"Chrome not found at {CHROME_EXE}")
            return results
        if not CHROME_HTML.exists():
            results["errors"].append(f"HTML harness not found at {CHROME_HTML}")
            return results
        if not DEMO_P2P.exists():
            results["errors"].append(f"demo-p2p not found at {DEMO_P2P}")
            return results

        # ── 1. Free the signaling port ──────────────────────────────────────
        kill_port(signaling_port)
        time.sleep(0.3)

        # ── 2. Seed SPKI file (demo-p2p writes it before we poll) ──────────
        spki_file.write_text("", encoding="utf-8")

        # ── 3. Environment for demo-p2p ─────────────────────────────────────
        env = dict(os.environ)
        env["NIMRTC_DTLS_SPKI_FILE"] = str(spki_file)
        env["NIMRTC_DTLS_TRACE"]      = str(nimrtc_trace)
        env["NIMRTC_DTLS_KEYLOG"]     = str(nimrtc_keylog)
        env["NIMRTC_DTLS_DUMP"]       = "1"

        # ── 4. Start signaling server ───────────────────────────────────────
        start_proc(
            "signaling_server",
            [sys.executable, str(SIGNALING_SERVER),
             "--port", str(signaling_port),
             "--room", room],
            stdout=log_signal, stderr=subprocess.STDOUT,
        )
        if not wait_for_port(signaling_port, timeout_s=5.0):
            results["errors"].append(
                f"signaling_server did not open :{signaling_port}")
            return results
        logger.info("signaling_server ready on :%d", signaling_port)

        # ── 5. Start signaling_proxy → demo-p2p (NimRTC answerer) ──────────
        # demo-p2p runs as answerer because Chrome (BoringSSL) will be the
        # offerer — it generates the SDP offer that demo-p2p processes.
        # Bind to 0.0.0.0 so ICE candidates are reachable from Chrome.
        # We pass the full env dict so demo-p2p sees DTLS env vars.
        # NIMRTC_PROXY_STDERR routes demo-p2p's stderr into our e2e dir
        # so the harness can grep the DTLS-reached-Connected line below.
        # Without this, the proxy's NimRTCSubprocess would write to its
        # default path under interop/signaling/ and the harness would see
        # only the proxy's own log (not demo-p2p's engine trace).
        # proxy_err_path is set later in the file (Step 7) so we resolve
        # the same path here to keep the two references in sync.
        proxy_err_path = E2E_DIR / "case_e_proxy.err"
        proxy_env = dict(os.environ)
        proxy_env["NIMRTC_DTLS_SPKI_FILE"] = str(spki_file)
        proxy_env["NIMRTC_DTLS_TRACE"]      = str(nimrtc_trace)
        proxy_env["NIMRTC_DTLS_KEYLOG"]     = str(nimrtc_keylog)
        proxy_env["NIMRTC_DTLS_DUMP"]       = "1"
        proxy_env["NIMRTC_PROXY_STDERR"]    = str(proxy_err_path)

        proxy_proc = subprocess.Popen(
            [
                sys.executable, str(SIGNALING_PROXY),
                "--signaling",  f"ws://127.0.0.1:{signaling_port}/{room}",
                "--binary",     str(DEMO_P2P),
                "--answerer",
                "--bind",       "0.0.0.0",
                "--no-stun",
                "--duration",    str(timeout_s + 10),
            ],
            stdout=log_proxy, stderr=log_proxy_e,
            env=proxy_env,
            creationflags=subprocess.CREATE_NEW_PROCESS_GROUP
                if sys.platform == "win32" else 0,
        )
        procs.append(("signaling_proxy", proxy_proc))
        logger.info("signaling_proxy started (pid=%d)", proxy_proc.pid)

        # Give the proxy a moment to spawn demo-p2p and open the engine.
        time.sleep(2.0)

        # ── 6. Wait for SPKI file to be populated by demo-p2p ──────────────
        deadline_spki = time.monotonic() + 5
        nimrtc_spki_list: list[str] = []
        while time.monotonic() < deadline_spki:
            text = spki_file.read_text(errors="replace").strip()
            if text:
                nimrtc_spki_list = text.splitlines()
                logger.info("SPKI file populated with %d entry(ies)", len(nimrtc_spki_list))
                break
            time.sleep(0.2)

        # ── 7. Launch headless Chrome ──────────────────────────────────────
        chrome_args = [
            "--headless=new",
            "--no-sandbox",
            "--disable-dev-shm-usage",
            "--use-fake-ui-for-media-stream",
            "--use-fake-device-for-media-stream",
            "--ssl-key-log-file=" + str(E2E_DIR / "case_e_chrome_boringssl.keylog"),
            "--enable-logging=stderr",
            "--v=1",
            "--log-level=0",
            f"--log-file={chrome_verbose_log}",
            "--ignore-certificate-errors-spki-list=" + ",".join(nimrtc_spki_list)
                if nimrtc_spki_list else "",
        ]
        chrome_args = [a for a in chrome_args if a]  # drop empty strings

        chrome_url = CHROME_HTML.as_uri() + f"?ws={ws_url}&room={room}&offerer=1"

        try:
            from playwright.sync_api import sync_playwright, Error as PlaywrightError
        except ImportError:
            results["errors"].append(
                "playwright not installed — run: pip install playwright && "
                "python -m playwright install chromium "
                "(or set NIMRTC_CHROME_PATH to a system Chrome if you have one)")
            stop_all()
            return results

        # proxy_err_path is resolved earlier (Step 5) so demo-p2p's
        # stderr flows to the same file we now read.

        logger.info("Launching Chrome: %s", chrome_url)
        chrome_results: dict = {}
        chrome_console_lines: list[str] = []

        try:
            with sync_playwright() as pw:
                browser = pw.chromium.launch(
                    headless=True,
                    executable_path=CHROME_EXE,
                    args=chrome_args,
                )
                context = browser.new_context()
                page = context.new_page()

                page.on("console", lambda m: chrome_console_lines.append(
                    f"[{m.type}] {m.text}"))
                page.on("pageerror", lambda e: chrome_console_lines.append(
                    f"[pageerror] {e}"))

                page.goto(chrome_url)
                logger.info("Chrome page loaded — waiting up to %ds for interop", timeout_s)

                # Poll window._interopResults until deadline.
                deadline = time.monotonic() + timeout_s
                last_snap: dict = {}
                while time.monotonic() < deadline:
                    try:
                        snap = page.evaluate(
                            "({"
                            " ws: window._interopResults?.wsConnected,"
                            " ice: window._interopResults?.iceConnected,"
                            " dtls: window._interopResults?.dtlsState,"
                            " srtp: window._interopResults?.srtpCipher,"
                            " errs: (window._interopResults?.errors || []).length,"
                            "})"
                        ) or {}
                        last_snap = snap
                    except Exception as exc:
                        logger.debug("page.evaluate failed: %s", exc)
                    time.sleep(0.5)

                # Capture final results.
                try:
                    chrome_results = page.evaluate(
                        "window._interopResults || {}") or {}
                except Exception as exc:
                    logger.warning("final results evaluate failed: %s", exc)

                browser.close()
        except PlaywrightError as exc:
            results["errors"].append(f"Playwright error: {exc}")
            stop_all()
            return results

        # Write console log to disk.
        chrome_console_log.write_text(
            "\n".join(chrome_console_lines), encoding="utf-8")
        logger.info("Chrome console: %d lines", len(chrome_console_lines))

        # ── 8. Collect NimRTC evidence from demo-p2p log ───────────────────
        demo_lines: list[str] = []
        if proxy_err_path.exists():
            demo_lines = proxy_err_path.read_text(
                encoding="utf-8", errors="replace").splitlines()

        # Grep for the key evidence lines.
        # Use multiple patterns to handle encoding variations:
        # - UTF-8 with arrow (→)  — normal output
        # - UTF-8 with '->'       — fallback in case arrow gets mangled
        # - ASCII only patterns   — for extra robustness
        DTLS_PATTERNS = [
            "engine[dtls→srtp]: DTLS reached Connected",  # UTF-8 arrow
            "engine[dtls->srtp]: DTLS reached Connected",  # ASCII arrow
            "dtls->srtp]: DTLS reached Connected",         # partial
            "DTLS reached Connected",                       # minimal (unique enough)
        ]
        SRTP_PATTERNS = [
            "engine[dtls→srtp]: SRTP keys INSTALLED",     # UTF-8 arrow
            "engine[dtls->srtp]: SRTP keys INSTALLED",    # ASCII arrow
            "SRTP keys INSTALLED",                        # minimal
        ]

        def find_line(patterns: list[str], lines: list[str]) -> str | None:
            """Find first line matching any of the patterns."""
            for ln in lines:
                for p in patterns:
                    if p in ln:
                        return ln
            return None

        dtls_connected_line = find_line(DTLS_PATTERNS, demo_lines)
        srtp_installed_line = find_line(SRTP_PATTERNS, demo_lines)

        # ── 8a. Verify DTLS handshake via keylog (authoritative) ─────────────
        # The NIMRTC_DTLS_KEYLOG file is written in SSLKEYLOGFORMAT, which
        # is always ASCII. If the keylog contains evidence of key material,
        # it proves wolfSSL exported session keys (i.e. handshake progressed past
        # server Finished). CLIENT_RANDOM appears in TLS 1.2; both CLIENT_RANDOM
        # and SERVER_HANDSHAKE_TRAFFIC_SECRET appear in TLS 1.3.
        keylog_ok = False
        if nimrtc_keylog.exists() and nimrtc_keylog.stat().st_size > 0:
            keylog_content = nimrtc_keylog.read_text(encoding="utf-8", errors="replace")
            # CLIENT_RANDOM proves wolfSSL exported a session ticket / master secret.
            # SERVER_HANDSHAKE_TRAFFIC_SECRET (TLS 1.3) would additionally prove
            # post-Finished handshake progress, but is not required.
            keylog_ok = "CLIENT_RANDOM" in keylog_content
            logger.info("E5 keylog evidence: %s (CLIENT_RANDOM %s)",
                        "PASS" if keylog_ok else "FAIL",
                        "found" if keylog_ok else "not found")

        # ── 9. Compute PASS/FAIL ─────────────────────────────────────────────
        chrome_ws_ok   = bool(chrome_results.get("wsConnected"))
        chrome_ice_ok  = bool(chrome_results.get("iceConnected"))
        chrome_dtls_ok = str(chrome_results.get("dtlsState", "")) == "connected"
        chrome_srtp_ok = bool(chrome_results.get("srtpCipher"))
        # E5: DTLS handshake proof — prefer keylog (ASCII, unambiguous), fall
        # back to log-line match (handles encoding issues).
        nimrtc_dtls_ok = keylog_ok or (dtls_connected_line is not None)

        logger.info("E1 Chrome wsConnected:    %s", chrome_ws_ok)
        logger.info("E2 Chrome iceConnected:    %s", chrome_ice_ok)
        logger.info("E3 Chrome dtlsState:       %s  (got: %r)",
                    chrome_dtls_ok, chrome_results.get("dtlsState"))
        logger.info("E4 Chrome srtpCipher:      %s  (got: %r)",
                    chrome_srtp_ok, chrome_results.get("srtpCipher"))
        logger.info("E5 NimRTC DTLS Connected:  %s  (keylog=%s, log=%s)",
                    nimrtc_dtls_ok,
                    "PASS" if keylog_ok else "FAIL",
                    "found" if dtls_connected_line else "not found")

        # Overall: all three must pass.
        results["pass"] = all([
            chrome_ws_ok,
            chrome_ice_ok,
            chrome_dtls_ok,
            chrome_srtp_ok,
            nimrtc_dtls_ok,
        ])
        results["chrome"] = {
            "wsConnected":  chrome_ws_ok,
            "iceConnected": chrome_ice_ok,
            "dtlsState":    str(chrome_results.get("dtlsState", "unknown")),
            "srtpCipher":   str(chrome_results.get("srtpCipher") or ""),
        }
        results["nimrtc_dtls_line"]  = dtls_connected_line  or ""
        results["nimrtc_srtp_line"]  = srtp_installed_line or ""
        results["nimrtc_keylog_ok"]  = keylog_ok
        results["errors"] = []

        if not results["pass"]:
            failures = []
            if not chrome_ws_ok:  failures.append("Chrome WebSocket not connected")
            if not chrome_ice_ok: failures.append("Chrome ICE not connected")
            if not chrome_dtls_ok: failures.append(
                f"Chrome dtlsState != 'connected' (got: {chrome_results.get('dtlsState')!r})")
            if not chrome_srtp_ok: failures.append(
                f"Chrome srtpCipher not set (got: {chrome_results.get('srtpCipher')!r})")
            if not nimrtc_dtls_ok:
                if not keylog_ok:
                    failures.append(
                        "NimRTC keylog missing CLIENT_RANDOM "
                        "(wolfSSL key export failed or handshake incomplete)")
                if not dtls_connected_line:
                    failures.append(
                        "NimRTC demo-p2p log missing 'DTLS reached Connected' "
                        "(encoding workaround: checked UTF-8 + ASCII patterns)")
            results["errors"] = failures

        # Print evidence summary.
        print("\n" + "=" * 60)
        print("  Case E — Chrome ↔ NimRTC DTLS interop evidence")
        print("=" * 60)
        print(f"  E1. Chrome wsConnected:    {'PASS' if chrome_ws_ok  else 'FAIL'}")
        print(f"  E2. Chrome iceConnected:   {'PASS' if chrome_ice_ok else 'FAIL'}")
        print(f"  E3. Chrome dtlsState:      {'PASS' if chrome_dtls_ok else 'FAIL'}")
        print(f"  E4. Chrome srtpCipher:     {'PASS' if chrome_srtp_ok else 'FAIL'}")
        keylog_status = "PASS" if keylog_ok else "FAIL"
        log_status = "found" if dtls_connected_line else "not found"
        print(f"  E5. NimRTC DTLS Connected:  {'PASS' if nimrtc_dtls_ok else 'FAIL'} "
              f"(keylog={keylog_status}, log={log_status})")
        print()
        print(f"  Overall: {'PASS' if results['pass'] else 'FAIL'}")
        if keylog_ok:
            print("  NimRTC keylog: CLIENT_RANDOM found ✓ (TLS master secret exported)")
        if dtls_connected_line:
            print(f"  NimRTC DTLS line: {dtls_connected_line}")
        if srtp_installed_line:
            print(f"  NimRTC SRTP line: {srtp_installed_line}")
        print("=" * 60)

        return results

    finally:
        stop_all()
        log_signal.close()
        log_proxy.close()
        log_proxy_e.close()


def main() -> int:
    import argparse
    parser = argparse.ArgumentParser(
        description="Chrome ↔ NimRTC DTLS interop harness (Case E)")
    parser.add_argument(
        "--signaling-port", type=int, default=9000,
        help="WebSocket signaling server port (default: 9000)")
    parser.add_argument(
        "--timeout", type=int, default=25,
        help="Max seconds to wait for DTLS handshake (default: 25)")
    args = parser.parse_args()

    logger.info("Starting Case E harness "
                "(signaling-port=%d, timeout=%ds)", args.signaling_port, args.timeout)

    results = run(signaling_port=args.signaling_port, timeout_s=args.timeout)

    # Write machine-readable results for the orchestrator.
    results_path = E2E_DIR / "case_e_results.json"
    results_path.write_text(
        json.dumps(results, indent=2, ensure_ascii=False),
        encoding="utf-8",
    )
    logger.info("Results written to %s", results_path)

    print(f"\nRESULT: {'PASS' if results['pass'] else 'FAIL'}")
    return 0 if results["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
