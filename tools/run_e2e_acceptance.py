#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
tools/run_e2e_acceptance.py - Orchestrate the full end-to-end acceptance run.

Order:
  1. case_a_gcm_aead       - BCrypt AES-128-GCM round-trip (in-process)
  2. case_b_dtls_inproc    - NimRTC aes_gcm_seal/open (in-process)
  3. case_c_loopback       - NimRTC <-> NimRTC DTLS+ICE+SRTP (loopback UDP)
  4. case_d_chrome         - NimRTC <-> real Chrome via signaling server + Playwright

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

# Fix Windows console Unicode output: on Windows (GitHub Actions runner),
# the console encoding defaults to the system code page (cp1252) which
# cannot encode many Unicode characters that test binaries emit.  Reconfiguring
# stdout to UTF-8 lets Python's print() pass through Unicode strings without
# choking on arrows, box-drawing chars, etc.
if sys.stdout is not None:
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass  # CI redirects stdout to file where encoding doesn't matter

ROOT = Path(__file__).resolve().parent.parent
E2E  = ROOT / "build" / "e2e"
E2E.mkdir(parents=True, exist_ok=True)

# Honor NIMRTC_BUILD_DIR so callers can point at out-of-tree builds.
# single-config (cmake --preset debug on Linux/macOS) puts binaries directly
# under the build dir; multi-config (MSVC) nests them under <Config>/.
NIMRTC_BUILD_DIR = Path(os.environ.get("NIMRTC_BUILD_DIR", ROOT / "build"))

def _resolve(cfg: str, sub: str) -> Path:
    """Locate <build>/<sub>/<cfg>/ for MSVC multi-config, or
    <build>/<sub>/ for single-config (cmake --preset debug on Linux/macOS).
    Tries both orders so a partial rebuild that left files in only one of
    the two possible layouts still finds the binary."""
    multi_first  = NIMRTC_BUILD_DIR / sub / cfg      # Visual Studio default
    multi_second = NIMRTC_BUILD_DIR / cfg / sub      # older / some presets
    single       = NIMRTC_BUILD_DIR / sub
    for candidate in (multi_first, multi_second, single):
        if candidate.exists():
            return candidate
    return multi_first   # best guess so the error message points somewhere

BIN_TESTS    = _resolve("Debug", "tests")
BIN_EXAMPLES = _resolve("Debug", "examples")

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
    print(f"\n[run] {exe.name} -> {log.name}")
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

        # Set DTLS diagnostic env vars BEFORE spawning the proxy so the
        # demo-p2p child (which the proxy Popen()s without env= and
        # therefore inherits the proxy's environment) sees them.
        os.environ["NIMRTC_DTLS_KEYLOG"] = str(E2E / "nimrtc_chrome.keylog")
        os.environ["NIMRTC_DTLS_TRACE"]  = str(E2E / "nimrtc_chrome.trace")
        os.environ["NIMRTC_DTLS_DUMP"]   = "1"
        # Without this, demo-p2p writes the SPKI to its fallback path
        # (interop/build/nimrtc_spki.txt) and the orchestrator can't
        # reliably locate it before launching Chrome.  Demo-p2p opens
        # (which loads the cert into wolfSSL) happens BEFORE the proxy
        # Popen, so we seed the file as empty and let demo-p2p overwrite
        # it once the engine is ready.
        os.environ["NIMRTC_DTLS_SPKI_FILE"] = str(E2E / "nimrtc_chrome_spki.b64")
        if not (E2E / "nimrtc_chrome_spki.b64").exists():
            (E2E / "nimrtc_chrome_spki.b64").write_text("", encoding="utf-8")

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

        # Real pass/fail is in the JSON block.  We assert on:
        #   wsConnected   — signaling plane
        #   iceConnected  — ICE completed
        #   sdpOfferSeen  — Chrome produced an offer (offerer side)
        #   audioReceived — Chrome actually received RTP audio (i.e. DTLS
        #                   handshake finished AND SRTP unwrap worked)
        #   rtpPackets    — sanity check that media flowed
        #   errors        — must be empty
        # We do NOT treat "ICE Connected but DTLS failed" as PASS — that was
        # a bug in earlier revisions of this script.
        ok = False
        results_obj: dict = {}
        try:
            text = chrome_log.read_text(errors="replace")
            # Look for the JSON results block between markers.
            marker = "=== Chrome interop results ==="
            i = text.find(marker)
            if i < 0:
                print("[FAIL] no results marker found in chrome log")
            else:
                tail_block = text[i + len(marker):].lstrip()
                # Find JSON object boundaries by counting braces.
                # The Chrome interop script dumps json.dumps(results, indent=2)
                # so the output spans multiple lines.  Simply finding the first
                # "}" would incorrectly truncate nested objects (e.g.
                # {"errors": []}).  Instead, we walk the string and count
                # nesting depth to find the matching closing brace.
                depth = 0
                start = -1
                end = -1
                for idx, ch in enumerate(tail_block):
                    if ch == '{':
                        if start < 0:
                            start = idx
                        depth += 1
                    elif ch == '}':
                        depth -= 1
                        if depth == 0:
                            end = idx + 1
                            break
                if start < 0 or end < 0:
                    print("[FAIL] could not locate JSON braces in results block")
                else:
                    cand = tail_block[start:end]
                    try:
                        results_obj = json.loads(cand)
                    except json.JSONDecodeError as e:
                        print(f"[FAIL] JSON decode error: {e}")
        except Exception as e:
            print(f"[FAIL] error parsing chrome log: {e}")

        if results_obj:
            checks = {
                "wsConnected":   results_obj.get("wsConnected")  is True,
                "iceConnected":  results_obj.get("iceConnected") is True,
                "sdpOfferSeen":  results_obj.get("sdpOfferSeen") is True,
                "audioReceived": results_obj.get("audioReceived") is True,
                "rtpPackets>0":  (results_obj.get("rtpPackets") or 0) > 0,
                "errors==[]":    not results_obj.get("errors"),
            }
            for name, passed in checks.items():
                print(f"   | check {name:18s}: {'OK' if passed else 'FAIL'}")
            ok = all(checks.values())

        if not ok and rc_chrome == 0:
            print("[FAIL] chrome script exit=0 but result assertions FAILED")
        rc_overall = rc_chrome
        return ok

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
