#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
tests/chrome_interop_harness/run_webtransport_harness.py
=======================================================
Chrome ↔ NimRTC WebTransport interop harness (Case E, WebTransport plane).

Architecture
------------
  ┌──────────────────────┐
  │  demo-wtserver        │  UDP :9443 (msquic listener)
  │  (WebTransport server)│  Echoes: datagrams + bidirectional streams
  └──────────┬───────────┘
             │ QUIC + HTTP/3 + WebTransport (RFC 9298)
  ┌──────────┴──────────────────────┐
  │  Chrome (headless)             │  WebTransport client
  │  test_chrome_webtransport.html │  RFC 8440 upgrade
  └────────────────────────────────┘

Evidence items (all must pass for overall PASS):
  E1. Chrome WebTransport API available (typeof WebTransport === 'function').
  E2. session.ready resolves → connected state.
  E3. Datagram sent → datagram echoed back (RFC 9221 plane).
  E4. Bidirectional stream opened → stream data echoed back (reliable plane).

The server certificate is self-signed; Chrome is launched with
--ignore-certificate-errors so it accepts localhost:9443 without a
trusted CA.  This is safe for a local test harness only.

Usage
-----
  python tests/chrome_interop_harness/run_webtransport_harness.py
      --wt-port 9443    (default)
      --timeout 20      (default: seconds to wait for handshake + data exchange)

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

WT_HTML    = ROOT / "interop" / "chrome" / "test_chrome_webtransport.html"

# Default to system Chrome; allow override via env var so CI / dev machines
# without Google Chrome (but with Playwright's bundled Chromium) can still
# run Case E.
_CHROME_DEFAULT = Path(r"C:\Program Files\Google\Chrome\Application\chrome.exe")
CHROME_EXE = Path(os.environ.get("NIMRTC_CHROME_PATH", str(_CHROME_DEFAULT)))

# demo-wtserver binary — resolve using the same logic as run_e2e_acceptance.py.
_NIMRTC_BUILD_DIR = Path(os.environ.get("NIMRTC_BUILD_DIR", BUILD_DIR))
for _sub, _cfg in [("examples", "Debug"), ("Debug", "examples"), ("examples", "")]:
    _p = _NIMRTC_BUILD_DIR / _sub / _cfg / "demo-wtserver.exe"
    if _p.exists():
        DEMO_WTSERVER = _p
        break
else:
    DEMO_WTSERVER = _NIMRTC_BUILD_DIR / "examples" / "Debug" / "demo-wtserver.exe"

# ─── Logging ───────────────────────────────────────────────────────────────────

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)-8s] %(name)s: %(message)s",
)
logger = logging.getLogger("case_e_webtransport_harness")

# ─── Helpers ──────────────────────────────────────────────────────────────────


def wait_for_port(port: int, timeout_s: float = 5.0) -> bool:
    """Poll until something is listening on localhost:port (UDP or TCP)."""
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def wait_for_string_in_file(path: Path, needle: str, timeout_s: float = 10.0) -> bool:
    """Poll a text file until `needle` appears in it."""
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            text = path.read_text(errors="replace")
            if needle in text:
                return True
        except FileNotFoundError:
            pass
        time.sleep(0.2)
    return False


def kill_process_on_port(port: int) -> None:
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


def run(wt_port: int = 9443, timeout_s: int = 20) -> dict:
    """
    Run the Chrome ↔ NimRTC WebTransport interop test.

    Returns a dict with:
      pass          bool
      apiAvailable  bool
      connected     bool
      datagramSent  bool
      datagramRecv  bool
      streamSent    bool
      streamRecv    bool
      errors        list[str]
    """
    results = {
        "pass":         False,
        "apiAvailable": False,
        "connected":    False,
        "datagramSent": False,
        "datagramRecv": False,
        "datagramPayload": None,
        "streamSent":   False,
        "streamRecv":   False,
        "streamPayload": None,
        "closed":       False,
        "errors":       [],
    }

    wt_html_url = WT_HTML.as_uri() + f"?wtport={wt_port}&timeout={timeout_s * 1000}"
    server_log  = E2E_DIR / "case_e_wtserver.log"
    chrome_console_log = E2E_DIR / "case_e_wt_chrome_console.log"
    chrome_verbose_log  = E2E_DIR / "case_e_wt_chrome_verbose.log"

    procs: list[tuple[str, subprocess.Popen]] = []

    def start_proc(name: str, argv, stdout_path: Path, stderr_path: Path,
                  env: dict | None = None) -> subprocess.Popen:
        stdout_f = open(stdout_path, "w", encoding="utf-8")
        stderr_f = open(stderr_path, "w", encoding="utf-8")
        kw = dict(stdout=stdout_f, stderr=stderr_f)
        if sys.platform == "win32":
            kw["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
        # Inherit full environment, optionally overlaid with custom vars.
        import os as _os
        full_env = dict(_os.environ)
        if env:
            full_env.update(env)
        kw["env"] = full_env
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
        kill_process_on_port(wt_port)

    try:
        # ── 0. Verify prerequisites ──────────────────────────────────────────
        if not CHROME_EXE.exists():
            results["errors"].append(f"Chrome not found at {CHROME_EXE}")
            return results
        if not WT_HTML.exists():
            results["errors"].append(f"WebTransport HTML not found at {WT_HTML}")
            return results
        if not DEMO_WTSERVER.exists():
            results["errors"].append(f"demo-wtserver not found at {DEMO_WTSERVER}")
            return results

        # ── 1. Free the port ────────────────────────────────────────────────
        kill_process_on_port(wt_port)
        time.sleep(0.3)

        # ── 2. Start demo-wtserver ─────────────────────────────────────────
        server_log_path  = E2E_DIR / "case_e_wtserver.out"
        server_err_path = E2E_DIR / "case_e_wtserver.err"
        start_proc(
            "demo-wtserver",
            [str(DEMO_WTSERVER), "--port", str(wt_port), "--duration", str(timeout_s + 15)],
            server_log_path,
            server_err_path,
            # Tell demo-wtserver where to find wtserver_cert.pfx.
            env={"NIMRTC_BUILD_ROOT": str(BUILD_DIR)},
        )

        # Wait for the server to confirm startup (it prints a message to stderr).
        if not wait_for_string_in_file(server_err_path,
                                        "server started on UDP",
                                        timeout_s=8.0):
            err_text = server_err_path.read_text(errors="replace")
            results["errors"].append(
                f"demo-wtserver did not start successfully.\n"
                f"  stderr excerpt: {err_text[-500:]}")
            return results
        logger.info("demo-wtserver running on UDP :%d", wt_port)

        # Give the msquic listener a moment to fully bind.
        time.sleep(0.5)

        # ── 3. Launch headless Chrome ───────────────────────────────────────
        chrome_args = [
            "--headless=new",
            "--no-sandbox",
            "--disable-dev-shm-usage",
            "--use-fake-ui-for-media-stream",
            # Self-signed cert on localhost:9443 — accept it unconditionally.
            # This is safe ONLY for a local test harness.  Do NOT use in
            # production or open networks.
            "--ignore-certificate-errors",
            "--allow-running-insecure-content",
            "--enable-logging=stderr",
            "--v=1",
            "--log-level=0",
            f"--log-file={chrome_verbose_log}",
        ]
        chrome_url = wt_html_url

        logger.info("Launching Chrome: %s", chrome_url)
        chrome_results: dict = {}
        chrome_console_lines: list[str] = []

        try:
            from playwright.sync_api import sync_playwright, Error as PlaywrightError
        except ImportError:
            results["errors"].append(
                "playwright not installed — run: pip install playwright && "
                "python -m playwright install chromium "
                "(or set NIMRTC_CHROME_PATH to a system Chrome if you have one)")
            stop_all()
            return results

        logger.info("Launching Chrome with WebTransport harness: %s", chrome_url)
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
                logger.info("Chrome page loaded — waiting up to %ds for interop",
                            timeout_s)

                # Poll window._interopResults until deadline.
                deadline = time.monotonic() + timeout_s
                last_snap: dict = {}
                while time.monotonic() < deadline:
                    try:
                        snap = page.evaluate(
                            "({"
                            " api:   window._interopResults?.apiAvailable,"
                            " conn:  window._interopResults?.connected,"
                            " ds:    window._interopResults?.datagramSent,"
                            " dr:    window._interopResults?.datagramRecv,"
                            " ss:    window._interopResults?.streamSent,"
                            " sr:    window._interopResults?.streamRecv,"
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

        # ── 4. Collect demo-wtserver log lines ──────────────────────────────
        wtserver_err = server_err_path.read_text(errors="replace").splitlines()

        # ── 5. Compute PASS/FAIL ───────────────────────────────────────────
        api_ok       = bool(chrome_results.get("apiAvailable"))
        connected_ok = bool(chrome_results.get("connected"))
        dg_sent_ok   = bool(chrome_results.get("datagramSent"))
        dg_recv_ok   = bool(chrome_results.get("datagramRecv"))
        st_sent_ok   = bool(chrome_results.get("streamSent"))
        st_recv_ok   = bool(chrome_results.get("streamRecv"))

        logger.info("E1 Chrome apiAvailable:    %s", api_ok)
        logger.info("E2 Chrome connected:      %s", connected_ok)
        logger.info("E3 Datagram sent:        %s", dg_sent_ok)
        logger.info("E4 Datagram recv/echo:   %s", dg_recv_ok)
        logger.info("E5 Stream sent:          %s", st_sent_ok)
        logger.info("E6 Stream recv/echo:    %s", st_recv_ok)

        results["pass"]         = all([api_ok, connected_ok, dg_sent_ok,
                                        dg_recv_ok, st_sent_ok, st_recv_ok])
        results["apiAvailable"] = api_ok
        results["connected"]    = connected_ok
        results["datagramSent"] = dg_sent_ok
        results["datagramRecv"] = dg_recv_ok
        results["datagramPayload"] = chrome_results.get("datagramPayload")
        results["streamSent"]   = st_sent_ok
        results["streamRecv"]   = st_recv_ok
        results["streamPayload"] = chrome_results.get("streamPayload")
        results["closed"]       = bool(chrome_results.get("closed"))

        if not results["pass"]:
            failures = []
            if not api_ok:       failures.append("Chrome WebTransport API not available")
            if not connected_ok: failures.append(
                f"WebTransport session did not reach connected state (got: "
                f"{chrome_results.get('connected')!r})")
            if not dg_sent_ok:   failures.append("Datagram was not sent by Chrome")
            if not dg_recv_ok:   failures.append("Datagram echo was not received")
            if not st_sent_ok:   failures.append("Bidirectional stream data not sent")
            if not st_recv_ok:   failures.append("Bidirectional stream echo not received")
            results["errors"] = failures

        # Print evidence summary.
        print("\n" + "=" * 60)
        print("  Case E — Chrome ↔ NimRTC WebTransport interop evidence")
        print("=" * 60)
        print(f"  E1. Chrome apiAvailable:   {'PASS' if api_ok       else 'FAIL'}")
        print(f"  E2. Session connected:    {'PASS' if connected_ok else 'FAIL'}")
        print(f"  E3. Datagram sent:        {'PASS' if dg_sent_ok   else 'FAIL'}")
        print(f"  E4. Datagram echo recv:   {'PASS' if dg_recv_ok   else 'FAIL'}")
        print(f"  E5. Stream sent:          {'PASS' if st_sent_ok   else 'FAIL'}")
        print(f"  E6. Stream echo recv:     {'PASS' if st_recv_ok   else 'FAIL'}")
        print()
        print(f"  Overall: {'PASS' if results['pass'] else 'FAIL'}")
        if wtserver_err:
            wtserver_start = [l for l in wtserver_err if "server started" in l]
            if wtserver_start:
                print(f"\n  demo-wtserver: {wtserver_start[0]}")
            wtserver_session = [l for l in wtserver_err if "new session" in l]
            for l in wtserver_session:
                print(f"  demo-wtserver: {l}")
        print("=" * 60)

        return results

    finally:
        stop_all()


def main() -> int:
    import argparse
    parser = argparse.ArgumentParser(
        description="Chrome ↔ NimRTC WebTransport interop harness (Case E)")
    parser.add_argument(
        "--wt-port", type=int, default=9443,
        help="WebTransport UDP port (default: 9443)")
    parser.add_argument(
        "--timeout", type=int, default=20,
        help="Max seconds to wait for handshake + data exchange (default: 20)")
    args = parser.parse_args()

    logger.info("Starting Case E WebTransport harness "
                "(wt-port=%d, timeout=%ds)", args.wt_port, args.timeout)

    results = run(wt_port=args.wt_port, timeout_s=args.timeout)

    # Write machine-readable results for the orchestrator.
    results_path = E2E_DIR / "case_e_webtransport_results.json"
    results_path.write_text(
        json.dumps(results, indent=2, ensure_ascii=False),
        encoding="utf-8",
    )
    logger.info("Results written to %s", results_path)

    print(f"\nRESULT: {'PASS' if results['pass'] else 'FAIL'}")
    return 0 if results["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
