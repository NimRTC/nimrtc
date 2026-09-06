#!/usr/bin/env python3
"""
tools/run_e2e_acceptance.py — Orchestrate the full end-to-end acceptance run.

Order:
  1. case_a_gcm_aead       — BCrypt AES-128-GCM round-trip (in-process)
  2. case_b_dtls_inproc    — NimRTC aes_gcm_seal/open (in-process)
  3. case_c_loopback       — NimRTC <-> NimRTC DTLS+ICE+SRTP (loopback UDP)
  4. case_d_chrome         — NimRTC <-> real Chrome via signaling server + Playwright

All artifacts go to build/e2e/. Output is captured to
build/e2e/case_<x>_*.log so the project root stays clean.

Usage:
    python tools/run_e2e_acceptance.py
"""

from __future__ import annotations

import json
import os
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
E2E  = ROOT / "build" / "e2e"
E2E.mkdir(parents=True, exist_ok=True)

BIN_TESTS    = ROOT / "build" / "tests"   / "Debug"
BIN_EXAMPLES = ROOT / "build" / "examples" / "Debug"

EXE_GCM     = BIN_TESTS  / "test_dtls_gcm_aead.exe"
EXE_INPROC  = BIN_TESTS  / "test_dtls_inproc.exe"
EXE_LOOP    = BIN_EXAMPLES / "loopback-p2p.exe"
EXE_DEMO    = BIN_EXAMPLES / "demo-p2p.exe"

SIGNALING_SERVER = ROOT / "interop" / "signaling" / "signaling_server.py"
SIGNALING_PROXY  = ROOT / "interop" / "signaling" / "signaling_proxy.py"

SIGNAL_PORT      = 8765
PROXY_DURATION   = 30


# ---------------------------------------------------------------------------
# Cases A / B / C — pure local executables
# ---------------------------------------------------------------------------

def run_local(exe: Path, log_name: str, timeout_s: int = 60) -> bool:
    log = E2E / log_name
    print(f"\n[run] {exe.name} → {log.name}")
    if not exe.exists():
        print(f"[FAIL] binary missing: {exe}")
        return False
    try:
        with open(log, "w", encoding="utf-8") as f:
            rc = subprocess.call(
                [str(exe)],
                stdout=f,
                stderr=subprocess.STDOUT,
                timeout=timeout_s,
            )
    except subprocess.TimeoutExpired:
        print(f"[FAIL] {exe.name} timed out after {timeout_s}s")
        return False

    # tail the log so the orchestrator output is informative
    if log.exists():
        tail = log.read_text(errors="replace").splitlines()[-12:]
        for ln in tail:
            print(f"   | {ln}")
    print(f"[rc]  {exe.name} exit={rc}")
    return rc == 0


# ---------------------------------------------------------------------------
# Case D — Chrome interop via signaling_server + signaling_proxy + Chrome
# ---------------------------------------------------------------------------

def wait_for_port(port: int, timeout_s: float = 5.0) -> bool:
    """Poll until something is listening on localhost:port."""
    import socket
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


def run_chrome_interop() -> bool:
    print("\n[run] case D: NimRTC <-> Chrome interop")

    log_signal  = open(E2E / "case_d_signal_server.log", "w", encoding="utf-8")
    log_proxy   = open(E2E / "case_d_proxy.out", "w", encoding="utf-8")
    log_proxy_e = open(E2E / "case_d_proxy.err", "w", encoding="utf-8")

    procs: list[tuple[str, subprocess.Popen]] = []

    def _start(name: str, argv, stdout=None, stderr=None) -> subprocess.Popen:
        kw = dict(stdout=stdout or subprocess.DEVNULL,
                  stderr=stderr  or subprocess.DEVNULL)
        # Detach into its own process group on Windows so we can kill cleanly.
        kw["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
        p = subprocess.Popen(argv, **kw)
        procs.append((name, p))
        print(f"   started {name} (pid={p.pid})")
        return p

    rc_overall = 1
    try:
        # Free the port first.
        kill_port(SIGNAL_PORT)
        time.sleep(0.3)

        # 1) Signaling server
        _start(
            "signaling_server",
            [sys.executable, str(SIGNALING_SERVER), "--port", str(SIGNAL_PORT)],
            stdout=log_signal, stderr=subprocess.STDOUT,
        )
        if not wait_for_port(SIGNAL_PORT, timeout_s=5.0):
            print(f"[FAIL] signaling_server did not open :{SIGNAL_PORT}")
            return False
        print(f"   signaling_server listening on :{SIGNAL_PORT}")

        # 2) signaling_proxy (manages demo-p2p as NimRTC side; ANSWERER,
        # because e2e_chrome_interop.py loads Chrome as offerer in room
        # `interop`).
        #
        # Bind to 0.0.0.0 so the ICE host candidate is on a real NIC that
        # Chrome can reach (Chrome can't reach 127.0.0.1 since it lives in
        # a different network namespace under Playwright).
        # --no-stun disables the external STUN server — we have local host
        # candidates, and the external stun.l.google.com is unreliable here.
        _start(
            "signaling_proxy",
            [
                sys.executable, str(SIGNALING_PROXY),
                "--signaling", f"ws://127.0.0.1:{SIGNAL_PORT}/interop",
                "--binary", str(EXE_DEMO),
                "--answerer",
                "--bind", "0.0.0.0",
                "--no-stun",
                "--duration", str(PROXY_DURATION),
            ],
            stdout=log_proxy, stderr=log_proxy_e,
        )

        # Give the proxy a moment to spawn demo-p2p.
        time.sleep(1.5)

        # 3) Playwright Chrome
        # tools/e2e_chrome_interop.py takes care of launching headless Chrome
        # and pulling `window._interopResults`. We run it as a subprocess so
        # its full output appears in build/e2e/case_d_chrome.log.
        chrome_log = E2E / "case_d_chrome.log"
        cmd = [sys.executable, "-u", str(ROOT / "tools" / "e2e_chrome_interop.py")]
        with open(chrome_log, "w", encoding="utf-8") as f:
            rc_chrome = subprocess.call(cmd, stdout=f, stderr=subprocess.STDOUT,
                                        timeout=PROXY_DURATION + 30)
        # tail chrome log
        tail = chrome_log.read_text(errors="replace").splitlines()[-30:]
        for ln in tail:
            print(f"   | {ln}")
        print(f"[rc]  e2e_chrome_interop.py exit={rc_chrome}")

        rc_overall = rc_chrome
        return rc_chrome == 0

    finally:
        # Tear everything down (port-first, then process).
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
        kill_port(SIGNAL_PORT)
        log_signal.close()
        log_proxy.close()
        log_proxy_e.close()


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def main() -> int:
    results = {}
    print(f"=== End-to-end acceptance ===")
    print(f"   artifacts: {E2E}")

    results["a_gcm_aead"] = run_local(EXE_GCM,    "case_a_gcm_aead.log")
    results["b_inproc"]   = run_local(EXE_INPROC, "case_b_dtls_inproc.log")
    results["c_loopback"] = run_local(EXE_LOOP,   "case_c_loopback.log", timeout_s=20)
    results["d_chrome"]   = run_chrome_interop()

    print("\n=== Summary ===")
    for k, v in results.items():
        print(f"   {k:12s}: {'PASS' if v else 'FAIL'}")
    overall = all(results.values())
    print(f"\nRESULT: {'PASS' if overall else 'FAIL'}")
    return 0 if overall else 1


if __name__ == "__main__":
    sys.exit(main())
