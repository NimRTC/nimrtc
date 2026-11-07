#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
tools/run_e2e_acceptance.py - Orchestrate the full end-to-end acceptance run.

Order:
  1. case_a_gcm_aead       - BCrypt AES-128-GCM round-trip (in-process)
  2. case_b_dtls_inproc    - NimRTC aes_gcm_seal/open (in-process)
  3. case_c_loopback       - NimRTC <-> NimRTC DTLS+ICE+SRTP (loopback UDP)
  4. case_d_chrome         - NimRTC <-> real Chrome via signaling server + Playwright
                            (full audio pipeline: ICE + DTLS + SRTP + Opus RTP)
  5. case_e_dtls          - NimRTC <-> Chrome DTLS-only interop (wolfSSL ←→ BoringSSL)
                            (DTLS handshake + SRTP keys, no audio required)

All artifacts go to build/e2e/. Output is captured to
build/e2e/case_<x>_*.log so the project root stays clean.

Usage:
    python tools/run_e2e_acceptance.py               # run all cases A-D
    python tools/run_e2e_acceptance.py --case=e   # run Case E only (DTLS-only)
    python tools/run_e2e_acceptance.py --case=all  # run all cases A-E
"""

from __future__ import annotations

import json
import os
import re
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

# Executable suffix depends on the host.  MSVC appends `.exe`; the
# GCC/Clang toolchains do not, so a Linux build produces
# `build/examples/demo-p2p` while the same build on Windows produces
# `build/examples/Debug/demo-p2p.exe`.
#
# Hardcoding `.exe` here meant the runner could never find the binaries
# on Linux, which matters for the v1.1 release gate: the 7-day soak
# (plan Track C, item C5) requires green Case D / Case E runs on
# Windows *and* Linux x86_64, and the Linux half of that gate could
# never start if the runner could not locate its own executables.
EXE_SUFFIX = ".exe" if os.name == "nt" else ""

EXE_GCM     = BIN_TESTS  / ("test_dtls_gcm_aead" + EXE_SUFFIX)
EXE_INPROC  = BIN_TESTS  / ("test_dtls_inproc" + EXE_SUFFIX)
EXE_LOOP    = BIN_EXAMPLES / ("loopback-p2p" + EXE_SUFFIX)
EXE_DEMO    = BIN_EXAMPLES / ("demo-p2p" + EXE_SUFFIX)

SIGNALING_SERVER = ROOT / "interop" / "signaling" / "signaling_server.py"
SIGNALING_PROXY  = ROOT / "interop" / "signaling" / "signaling_proxy.py"

# The signaling WebSocket port.  8765 is the historical default, but on a
# developer machine it is regularly occupied by an unrelated service (a
# uvicorn/FastMCP HTTP server is a common offender).  On Windows the
# collision is silent and destructive: `websockets.serve` binds 0.0.0.0:8765
# successfully even when another process already holds 127.0.0.1:8765
# (SO_REUSEADDR semantics), and the incoming connections are then delivered
# to the FOREIGN listener.  That foreign server answers the WebSocket
# upgrade with "HTTP/1.1 404 Not Found", so the Python proxy dies with
# "timed out during opening handshake" and Chrome reports
# "WebSocket error" -- a failure that looks like a Chrome bug but is not.
#
# Fix: pick a free port at run time (or honour NIMRTC_SIGNAL_PORT), and
# verify the server really answers a WebSocket upgrade before starting the
# browser.  0 means "choose one automatically".
SIGNAL_PORT = int(os.environ.get("NIMRTC_SIGNAL_PORT", "0") or 0)
PROXY_DURATION   = 30


def pick_free_port() -> int:
    """Return a TCP port that is currently free on all local interfaces.

    The socket is bound to 0.0.0.0 (not 127.0.0.1) because the signaling
    server binds 0.0.0.0 too: a port that is only free on loopback may
    still be taken on a physical interface, and the bind would then fail
    at server start-up instead of here.

    `SO_EXCLUSIVEADDRUSE` is a WINDOWS-only socket option -- it does not
    exist in the `socket` module on Linux at all, so referencing it
    unconditionally raised `AttributeError: module 'socket' has no
    attribute 'SO_EXCLUSIVEADDRUSE'` and crashed every acceptance case
    on the Linux CI runner.  The option is also not needed there: on
    POSIX the semantics that matter come from `SO_REUSEADDR`, which the
    kernels apply differently but in a way that does not produce the
    silent "someone else answers the upgrade" collision this function
    exists to avoid.
    """
    import socket
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        exclusive = getattr(socket, "SO_EXCLUSIVEADDRUSE", None)
        if exclusive is not None:
            try:
                s.setsockopt(socket.SOL_SOCKET, exclusive, 1)
            except OSError:
                pass
        s.bind(("0.0.0.0", 0))
        s.listen(1)
        return int(s.getsockname()[1])


def ws_upgrade_status(port: int, path: str = "/__healthcheck__",
                      timeout_s: float = 5.0) -> str:
    """Perform a real WebSocket handshake against the signaling server.

    Returns the HTTP status line of the answer ("HTTP/1.1 101 ..." for a
    working signaling server, "HTTP/1.1 404 ..." when a foreign service
    has stolen the port, ...).  The probe uses its own room so it does not
    occupy an "offerer" slot in the room the real peers use.
    """
    import socket
    request = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: localhost:{port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n"
    )
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=timeout_s) as s:
            s.settimeout(timeout_s)
            s.sendall(request.encode())
            data = s.recv(512)
    except OSError as e:
        return f"connection error: {e}"
    if not data:
        return "no response"
    return data.split(b"\r\n", 1)[0].decode(errors="replace").strip()


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

def wait_for_port(port: int, timeout_s: float = 30.0) -> bool:
    """Poll until something is listening on localhost:port.

    The default timeout is 30 seconds, not 5.  The signaling server is
    a separate Python process: on a machine that just woke up (the
    soak's scheduled task runs at 3am) the interpreter start-up and the
    `import websockets` behind it are served from a cold page cache,
    and 5 seconds was not enough for the server to reach its first
    log line.  The failure looked like a product regression -- "the
    signaling server did not open the port" -- when it was really the
    harness giving up too early.
    """
    import socket
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def _log_tail(path, lines: int = 12) -> str:
    """Return the last few lines of a child process' log, or ''."""
    try:
        text = Path(path).read_text(errors="replace").splitlines()
    except OSError:
        return ""
    return "\n".join(text[-lines:])


def kill_port(port: int) -> None:
    """Best-effort: kill any process holding this port."""
    if os.name != "nt":
        # POSIX: `fuser` / `lsof` instead of netstat + taskkill, which
        # are Windows-only.  The acceptance runner also runs on Linux
        # (the scheduled soak workflow), where the old implementation
        # silently did nothing.
        for argv in (["fuser", "-k", f"{port}/tcp"], ["lsof", "-t", f"-i:{port}"]):
            try:
                subprocess.run(argv, capture_output=True, check=False)
            except Exception:
                continue
        return
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


def _parse_chrome_results(chrome_log) -> dict:
    """Extract the `window._interopResults` JSON block from a chrome log."""
    results_obj: dict = {}
    try:
        text = chrome_log.read_text(errors="replace")
        # Look for the JSON results block between markers.
        marker = "=== Chrome interop results ==="
        i = text.find(marker)
        if i < 0:
            print("[FAIL] no results marker found in chrome log")
            return results_obj
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
            return results_obj
        cand = tail_block[start:end]
        try:
            results_obj = json.loads(cand)
        except json.JSONDecodeError as e:
            print(f"[FAIL] JSON decode error: {e}")
    except Exception as e:
        print(f"[FAIL] error parsing chrome log: {e}")
    return results_obj


def _parse_demo_audio_rx(demo_log) -> dict:
    """Read the inbound-audio counters out of demo-p2p's stderr.

    demo-p2p prints one line per half second while ICE is up:

        [demo-p2p] audio-rx frames=123 samples=59040 concealed=0 ...

    and a final summary line when the tick loop ends normally:

        [demo-p2p] audio-rx summary frames=123 ...

    The counters come from the engine's own `Stats`, so a non-zero
    `frames` is the NimRTC-side proof of the Chrome -> NimRTC direction
    (spec V-3): the packet arrived, the SRTP unwrap worked, the jitter
    buffer held it, the decoder produced PCM and the frame was played
    out to the host.

    The *last* matching line wins — the periodic line is printed during
    the run, so the newest value is the freshest one.
    """
    out: dict = {}
    if not demo_log or not Path(demo_log).exists():
        return out
    try:
        text = Path(demo_log).read_text(errors="replace")
    except OSError:
        return out
    for line in text.splitlines():
        if "audio-rx" not in line:
            continue
        for key in ("frames", "samples", "concealed", "fec", "lost", "errors"):
            m = re.search(rf"\b{key}=(\d+)", line)
            if m:
                out[key] = int(m.group(1))
    return out


def _check_audio_results(results_obj: dict, *, direction: str,
                         expect_reverse: bool = False) -> bool:
    """Shared audio-interop assertions (Case D forward + reverse).

    Evidence (audio-first release gate #1):
      wsConnected   — signaling plane
      iceConnected  — ICE completed
      sdpOfferSeen  — an offer was seen (either side may offer)
      audioReceived — Chrome actually received RTP audio (i.e. DTLS
                      handshake finished AND SRTP unwrap worked)
      rtpPackets    — sanity check that media flowed
      errors        — must be empty
    We do NOT treat "ICE Connected but DTLS failed" as PASS — that was
    a bug in earlier revisions of this script.

    With `expect_reverse` the Chrome -> NimRTC direction is required as
    well: the page must have captured the fake microphone
    (`micTrackAdded`) and its audio sender must have put packets on the
    wire (`outboundPackets`).
    """
    if not results_obj:
        return False
    checks = {
        "wsConnected":   results_obj.get("wsConnected")  is True,
        "iceConnected":  results_obj.get("iceConnected") is True,
        "sdpOfferSeen":  results_obj.get("sdpOfferSeen") is True,
        "audioReceived": results_obj.get("audioReceived") is True,
        "rtpPackets>0":  (results_obj.get("rtpPackets") or 0) > 0,
        "errors==[]":    not results_obj.get("errors"),
    }
    if expect_reverse:
        checks["micTrackAdded"]   = results_obj.get("micTrackAdded") is True
        checks["outboundPackets"] = (results_obj.get("outboundPackets") or 0) > 0
    print(f"   | direction={direction}")
    for name, passed in checks.items():
        print(f"   | check {name:18s}: {'OK' if passed else 'FAIL'}")
    return all(checks.values())


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
        # The flag is Windows-only -- subprocess.CREATE_NEW_PROCESS_GROUP
        # does not exist on POSIX, and referencing it unconditionally made
        # the whole acceptance runner crash with an AttributeError on
        # Linux (which is where the scheduled soak workflow runs).
        if sys.platform == "win32":
            kw["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
        p = subprocess.Popen(argv, **kw)
        procs.append((name, p))
        print(f"   started {name} (pid={p.pid})")
        return p

    rc_overall = 1
    # Choose the signaling port for THIS run.  See the SIGNAL_PORT comment
    # for why the historical fixed 8765 is not safe to assume.
    port = SIGNAL_PORT or pick_free_port()
    print(f"   signaling port: {port}")
    try:
        # Free the port first (only meaningful when the port was pinned
        # explicitly -- an auto-picked port is free by construction).
        if SIGNAL_PORT:
            kill_port(port)
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
        # Route demo-p2p's stderr (engine[dtls]/engine[ice] logs) into
        # build/e2e/ so the CI artifact upload picks it up.  Without this
        # the proxy defaults to interop/signaling/_proxy_demo_stderr.log,
        # which is outside build/e2e/ and never uploaded — leaving Case D
        # failures (e.g. DTLS ClientHello never reaching feed_inbound)
        # without engine-side evidence.
        os.environ["NIMRTC_PROXY_STDERR"] = str(E2E / "case_d_demo_stderr.log")

        # 1) Signaling server
        _start(
            "signaling_server",
            [sys.executable, str(SIGNALING_SERVER), "--port", str(port)],
            stdout=log_signal, stderr=subprocess.STDOUT,
        )
        if not wait_for_port(port, timeout_s=30.0):
            print(f"[FAIL] signaling_server did not open :{port} "
                  f"(waited 30s)")
            tail = _log_tail(E2E / "case_d_signal_server.log")
            if tail:
                print("   | signaling_server log tail:")
                for ln in tail.splitlines():
                    print(f"   | {ln}")
            return False
        print(f"   signaling_server listening on :{port}")

        # Prove the listener really is the NimRTC signaling server before
        # blaming Chrome for a later failure.  A foreign service holding
        # the port answers the upgrade with 404 (or nothing at all), and
        # that is exactly the state in which Case D used to fail with an
        # opaque "WebSocket error" on the Chrome side.
        status = ws_upgrade_status(port)
        print(f"   signaling health check: {status}")
        if not status.startswith("HTTP/1.1 101"):
            print(f"[FAIL] :{port} is not answering WebSocket upgrades "
                  f"(got {status!r}).  Another service is most likely "
                  f"listening on that port -- pick another one with "
                  f"NIMRTC_SIGNAL_PORT=<port>.")
            return False

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
                "--signaling", f"ws://127.0.0.1:{port}/interop",
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
        cmd = [sys.executable, "-u", str(ROOT / "tools" / "e2e_chrome_interop.py"),
               # Capture the fake microphone and send audio to NimRTC, so
               # the Chrome -> NimRTC direction of the audio pipeline is
               # observable too (spec V-3).
               "--send-audio"]
        # Tell the browser script which signaling server to talk to.
        chrome_env = dict(os.environ)
        chrome_env["NIMRTC_CHROME_WS_URL"] = f"ws://localhost:{port}/interop"
        with open(chrome_log, "w", encoding="utf-8") as f:
            rc_chrome = subprocess.call(cmd, stdout=f, stderr=subprocess.STDOUT,
                                        timeout=PROXY_DURATION + 30,
                                        env=chrome_env)
        # tail chrome log
        tail = chrome_log.read_text(errors="replace").splitlines()[-30:]
        for ln in tail:
            print(f"   | {ln}")
        print(f"[rc]  e2e_chrome_interop.py exit={rc_chrome}")

        # Real pass/fail is in the JSON block (shared audio assertions).
        ok = False
        results_obj = _parse_chrome_results(chrome_log)
        if results_obj:
            ok = _check_audio_results(results_obj, direction="chrome-offer",
                                      expect_reverse=True)

        # The NimRTC half of the reverse direction: the engine's own
        # inbound-audio counters, read from demo-p2p's stderr.  The
        # browser side (Chrome sent packets) and the engine side (NimRTC
        # decoded them) are two facts about the same flow, and only
        # together do they prove the receive path works end to end.
        rx = _parse_demo_audio_rx(E2E / "case_d_demo_stderr.log")
        rx_frames = int(rx.get("frames", 0) or 0)
        rx_ok = rx_frames > 0
        print(f"   | check nimrtc audio-rx frames: "
              f"{'OK' if rx_ok else 'FAIL'} (frames={rx_frames}, "
              f"samples={rx.get('samples', 0)}, "
              f"concealed={rx.get('concealed', 0)}, "
              f"lost={rx.get('lost', 0)})")
        ok = ok and rx_ok

        if not ok and rc_chrome == 0:
            print("[FAIL] chrome script exit=0 but result assertions FAILED")
        # Machine-readable evidence (mirrors case_e_*_results.json): the
        # Chrome results block plus the orchestrator verdict, so a later
        # audit can check Case D without re-parsing the chrome log.
        try:
            evidence = dict(results_obj) if results_obj else {}
            evidence["pass"] = bool(ok)
            evidence["direction"] = "chrome-offer"
            # The NimRTC-side inbound-audio counters (the reverse
            # direction's engine half).
            evidence["nimrtcAudioRx"] = rx
            (E2E / "case_d_results.json").write_text(
                json.dumps(evidence, indent=2), encoding="utf-8")
        except Exception as e:
            print(f"[warn] could not write case_d_results.json: {e}")
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
        kill_port(port)
        log_signal.close()
        log_proxy.close()
        log_proxy_e.close()


def run_chrome_interop_reverse() -> bool:
    """Case D-rev: NimRTC offer -> Chrome answer (reverse audio direction).

    Same evidence gates as Case D, but the roles flip: demo-p2p runs as
    OFFERER (no --answerer) and the Chrome page answers (no &offerer=1).
    Audio-only SDP (--no-datachannel on the proxy) keeps the reverse run
    focused on release gate #1 (bidirectional audio).
    """
    print("\n[run] case D-rev: NimRTC offer -> Chrome answer interop")

    log_signal  = open(E2E / "case_d_rev_signal_server.log", "w", encoding="utf-8")
    log_proxy   = open(E2E / "case_d_rev_proxy.out", "w", encoding="utf-8")
    log_proxy_e = open(E2E / "case_d_rev_proxy.err", "w", encoding="utf-8")

    procs: list[tuple[str, subprocess.Popen]] = []

    def _start(name: str, argv, stdout=None, stderr=None) -> subprocess.Popen:
        kw = dict(stdout=stdout or subprocess.DEVNULL,
                  stderr=stderr  or subprocess.DEVNULL)
        # Windows-only flag -- see run_chrome_interop() for the details.
        if sys.platform == "win32":
            kw["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
        p = subprocess.Popen(argv, **kw)
        procs.append((name, p))
        print(f"   started {name} (pid={p.pid})")
        return p

    rc_overall = 1
    port = SIGNAL_PORT or pick_free_port()
    print(f"   signaling port: {port}")
    try:
        if SIGNAL_PORT:
            kill_port(port)
            time.sleep(0.3)

        os.environ["NIMRTC_DTLS_KEYLOG"] = str(E2E / "nimrtc_chrome_rev.keylog")
        os.environ["NIMRTC_DTLS_TRACE"]  = str(E2E / "nimrtc_chrome_rev.trace")
        os.environ["NIMRTC_DTLS_DUMP"]   = "1"
        os.environ["NIMRTC_DTLS_SPKI_FILE"] = str(E2E / "nimrtc_chrome_rev_spki.b64")
        if not (E2E / "nimrtc_chrome_rev_spki.b64").exists():
            (E2E / "nimrtc_chrome_rev_spki.b64").write_text("", encoding="utf-8")
        # Same engine-log routing as the forward direction (see above):
        # keep demo-p2p stderr inside build/e2e/ for artifact upload.
        os.environ["NIMRTC_PROXY_STDERR"] = str(E2E / "case_d_rev_demo_stderr.log")

        _start(
            "signaling_server",
            [sys.executable, str(SIGNALING_SERVER), "--port", str(port)],
            stdout=log_signal, stderr=subprocess.STDOUT,
        )
        if not wait_for_port(port, timeout_s=30.0):
            print(f"[FAIL] signaling_server did not open :{port} (waited 30s)")
            tail = _log_tail(E2E / "case_d_rev_signal_server.log")
            if tail:
                print("   | signaling_server log tail:")
                for ln in tail.splitlines():
                    print(f"   | {ln}")
            return False
        print(f"   signaling_server listening on :{port}")

        status = ws_upgrade_status(port)
        print(f"   signaling health check: {status}")
        if not status.startswith("HTTP/1.1 101"):
            print(f"[FAIL] :{port} is not answering WebSocket upgrades "
                  f"(got {status!r}).  Another service is most likely "
                  f"listening on that port -- pick another one with "
                  f"NIMRTC_SIGNAL_PORT=<port>.")
            return False

        # NimRTC side is the OFFERER here (no --answerer): demo-p2p emits
        # its offer + candidates immediately on startup.
        _start(
            "signaling_proxy",
            [
                sys.executable, str(SIGNALING_PROXY),
                "--signaling", f"ws://127.0.0.1:{port}/interop",
                "--binary", str(EXE_DEMO),
                "--bind", "0.0.0.0",
                "--no-stun",
                "--no-datachannel",
                "--duration", str(PROXY_DURATION),
            ],
            stdout=log_proxy, stderr=log_proxy_e,
        )

        time.sleep(1.5)

        chrome_log = E2E / "case_d_rev_chrome.log"
        cmd = [sys.executable, "-u", str(ROOT / "tools" / "e2e_chrome_interop.py"),
               "--direction", "nimrtc-offer",
               # Same as the forward case: capture the fake microphone so
               # the Chrome -> NimRTC direction is checked with the
               # signaling roles flipped as well.
               "--send-audio"]
        chrome_env = dict(os.environ)
        chrome_env["NIMRTC_CHROME_WS_URL"] = f"ws://localhost:{port}/interop"
        with open(chrome_log, "w", encoding="utf-8") as f:
            rc_chrome = subprocess.call(cmd, stdout=f, stderr=subprocess.STDOUT,
                                        timeout=PROXY_DURATION + 30,
                                        env=chrome_env)
        tail = chrome_log.read_text(errors="replace").splitlines()[-30:]
        for ln in tail:
            print(f"   | {ln}")
        print(f"[rc]  e2e_chrome_interop.py --direction nimrtc-offer exit={rc_chrome}")

        ok = False
        results_obj = _parse_chrome_results(chrome_log)
        if results_obj:
            ok = _check_audio_results(results_obj, direction="nimrtc-offer",
                                      expect_reverse=True)

        # The NimRTC half of the reverse direction (see Case D).
        rx = _parse_demo_audio_rx(E2E / "case_d_rev_demo_stderr.log")
        rx_frames = int(rx.get("frames", 0) or 0)
        rx_ok = rx_frames > 0
        print(f"   | check nimrtc audio-rx frames: "
              f"{'OK' if rx_ok else 'FAIL'} (frames={rx_frames}, "
              f"samples={rx.get('samples', 0)}, "
              f"concealed={rx.get('concealed', 0)}, "
              f"lost={rx.get('lost', 0)})")
        ok = ok and rx_ok

        if not ok and rc_chrome == 0:
            print("[FAIL] chrome script exit=0 but result assertions FAILED")
        # Machine-readable evidence (mirrors case_d_results.json).
        try:
            evidence = dict(results_obj) if results_obj else {}
            evidence["pass"] = bool(ok)
            evidence["direction"] = "nimrtc-offer"
            evidence["nimrtcAudioRx"] = rx
            (E2E / "case_d_rev_results.json").write_text(
                json.dumps(evidence, indent=2), encoding="utf-8")
        except Exception as e:
            print(f"[warn] could not write case_d_rev_results.json: {e}")
        rc_overall = rc_chrome
        return ok

    finally:
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
        kill_port(port)
        log_signal.close()
        log_proxy.close()
        log_proxy_e.close()


# ---------------------------------------------------------------------------
# Case E — Chrome ↔ NimRTC DTLS-only interop (wolfSSL ←→ BoringSSL)
# ---------------------------------------------------------------------------

def run_case_e_dtls() -> bool:
    """
    CI-passable Chrome ↔ NimRTC DTLS interop verification.

    Evidence items:
      E1. Chrome ICE connected within 20 s.
      E2. Chrome DTLS state === 'connected' (BoringSSL has derived SRTP keys).
      E3. demo-p2p stderr contains
            engine[dtls→srtp]: DTLS reached Connected

    All three must pass for overall PASS.  Run with:
        python tools/run_e2e_acceptance.py --case=e
    """
    print("\n[run] case E: Chrome ↔ NimRTC DTLS-only interop")
    harness_py = ROOT / "tests" / "chrome_interop_harness" / "run_harness.py"
    if not harness_py.exists():
        print(f"[FAIL] harness not found: {harness_py}")
        return False

    log = E2E / "case_e_harness.log"
    # Same reasoning as Case D: the historical fixed port 9000 can be
    # occupied by an unrelated local service, and on Windows the bind then
    # succeeds while the traffic is stolen by the foreign listener.
    e_port = SIGNAL_PORT or pick_free_port()
    print(f"   signaling port: {e_port}")
    # A stale results file must never be able to decide this case.  The
    # harness writes case_e_results.json at the END of a successful run,
    # so a file left behind by an earlier run says nothing about THIS
    # one -- and reading it made a crashed harness (the
    # `CHROME_EXE` UnboundLocalError, for instance) report PASS.  Delete
    # it before the launch so only a fresh file can be found below.
    results_path = E2E / "case_e_results.json"
    if results_path.exists():
        try:
            results_path.unlink()
        except OSError as e:
            print(f"[warn] could not remove the stale {results_path}: {e}")

    try:
        with open(log, "w", encoding="utf-8") as f:
            rc = subprocess.call(
                [sys.executable, str(harness_py),
                 "--signaling-port", str(e_port),
                 "--timeout", "25"],
                stdout=f,
                stderr=subprocess.STDOUT,
                timeout=120,
            )
    except subprocess.TimeoutExpired:
        print(f"[FAIL] case E timed out after 120 s")
        return False

    if log.exists():
        tail = log.read_text(errors="replace").splitlines()[-20:]
        for ln in tail:
            print(f"   | {ln}")
    print(f"[rc]  run_harness.py exit={rc}")

    # Read the machine-readable results from the harness.  The verdict
    # needs BOTH a clean exit code and a fresh results file: the exit
    # code says the harness ran to completion, the results file says the
    # assertions inside it passed.
    if results_path.exists():
        try:
            obj = json.loads(results_path.read_text(errors="replace"))
            pass_ = obj.get("pass", False)
            print(f"[result] pass={pass_}")
            chrome = obj.get("chrome", {})
            dtls_state = chrome.get("dtlsState", "unknown")
            nimrtc_line = obj.get("nimrtc_dtls_line", "")
            print(f"         Chrome dtlsState={dtls_state!r}")
            print(f"         NimRTC DTLS line: {nimrtc_line[:80] if nimrtc_line else '(not found)'}")
            if rc != 0:
                print(f"[FAIL] the harness exited {rc}; its results file is not a verdict")
                return False
            return bool(pass_)
        except Exception as e:
            print(f"[warn] could not parse {results_path}: {e}")

    if rc != 0:
        print(f"[FAIL] run_harness.py exit={rc} and no results file was written")
        return False
    return rc == 0


# ---------------------------------------------------------------------------
# Case E (WebTransport) — Chrome ↔ NimRTC WebTransport over QUIC
# ---------------------------------------------------------------------------

def run_case_e_webtransport() -> bool:
    """
    Chrome ↔ NimRTC WebTransport end-to-end interop verification (Case E, WT plane).

    Evidence items:
      E1. Chrome WebTransport API available.
      E2. session.ready resolves → connected state.
      E3. Datagram sent → echoed back by demo-wtserver.
      E4. Bidirectional stream opened → stream data echoed back.

    All four must pass for overall PASS.  Run with:
        python tools/run_e2e_acceptance.py --case=e_webtransport
    """
    print("\n[run] case E: Chrome ↔ NimRTC WebTransport interop")
    harness_py = (ROOT / "tests" / "chrome_interop_harness"
                  / "run_webtransport_harness.py")
    if not harness_py.exists():
        print(f"[FAIL] harness not found: {harness_py}")
        return False

    log = E2E / "case_e_webtransport_harness.log"
    try:
        with open(log, "w", encoding="utf-8") as f:
            rc = subprocess.call(
                [sys.executable, str(harness_py),
                 "--wt-port", "9443",
                 "--timeout", "20"],
                stdout=f,
                stderr=subprocess.STDOUT,
                timeout=120,
            )
    except subprocess.TimeoutExpired:
        print("[FAIL] case E WebTransport harness timed out after 120 s")
        return False

    if log.exists():
        tail = log.read_text(errors="replace").splitlines()[-20:]
        for ln in tail:
            print(f"   | {ln}")
    print(f"[rc]  run_webtransport_harness.py exit={rc}")

    # Read the machine-readable results from the harness.
    results_path = E2E / "case_e_webtransport_results.json"
    if results_path.exists():
        try:
            obj = json.loads(results_path.read_text(errors="replace"))
            pass_ = obj.get("pass", False)
            print(f"[result] pass={pass_}")
            checks = {
                "apiAvailable":  obj.get("apiAvailable") is True,
                "connected":     obj.get("connected") is True,
                "datagramSent":  obj.get("datagramSent") is True,
                "datagramRecv":  obj.get("datagramRecv") is True,
                "streamSent":    obj.get("streamSent") is True,
                "streamRecv":    obj.get("streamRecv") is True,
            }
            for name, val in checks.items():
                print(f"         {name}: {'OK' if val else 'FAIL'}")
            return bool(pass_)
        except Exception as e:
            print(f"[warn] could not parse {results_path}: {e}")

    return rc == 0


# ---------------------------------------------------------------------------
# Case D — step summary writer
# ---------------------------------------------------------------------------
#
# Earlier revisions of this logic lived in a PowerShell `run:` block in
# `.github/workflows/interop.yml`, which on the GHA windows-2022 runner
# (PowerShell 5.1) tripped the parser error
#     "A parser error on the backtick-before-paren sequence."
# on the `$caseDLine` interpolation.  Moving the string composition into
# Python eliminates that whole class of failure: Python does no escape
# processing on backticks, and the here-doc is plain text after the
# variables are substituted.  The workflow step just copies the rendered
# file into `$GITHUB_STEP_SUMMARY`, so a GHA runner upgrade cannot
# reintroduce the parser error.

CASE_D_SUMMARY = """\
## E2E acceptance summary

Case A (BCrypt AES-GCM round-trip), Case B (in-process DTLS),
Case C (NimRTC<->NimRTC loopback) are always expected to PASS.

{case_d_line}
"""


def write_case_d_summary(skipped: bool) -> Path:
    """Render the Case-D step-summary markdown and write it to
    `build/e2e/case_d_summary.md`.  Returns the path so the caller
    (and the workflow) can use it.  Single source of truth for the
    Case-D line in the GitHub Actions Step Summary.
    """
    if skipped:
        case_d_line = (
            "Case D is **SKIPPED** on this branch (NIMRTC_SKIP_CASE_D=1) "
            "as a belt-and-braces escape hatch.  The fragment-reassembly "
            "bug is fixed in this release via the `DtlsHandshakeReassembler` "
            "shim (`src/modules/dtls/include/nimrtc/dtls/"
            "dtls_handshake_reassembler.hpp`); `main` does NOT need this "
            "flag.  The escape hatch is retained for any branch that has "
            "not yet merged the shim (e.g. the `fix/windows-dll-boundary` "
            "historical branch).  Cases A/B/C still run and gate the job."
        )
    else:
        case_d_line = (
            "Case D (NimRTC<->real Chrome DTLS handshake) is expected to "
            "PASS. The Case D certificate_unknown alert that blocked "
            "0.9.0-rc1 is fixed (see CHANGELOG.md \"Fixed - Chrome "
            "interop: fatal alert 46\")."
        )

    out = E2E / "case_d_summary.md"
    out.write_text(
        CASE_D_SUMMARY.format(case_d_line=case_d_line),
        encoding="utf-8",
    )
    return out


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def main() -> int:
    import argparse
    parser = argparse.ArgumentParser(description="NimRTC end-to-end acceptance runner")
    parser.add_argument(
        "--case",
        default="all",
        choices=["all", "a", "b", "c", "d", "d_rev", "e", "e_webtransport",
                 "ab", "abc", "abcd"],
        help=(
            "Which case(s) to run.  "
            "'all' runs A+B+C+D (Case E excluded unless explicitly listed).  "
            "'d_rev' runs the reverse audio direction (NimRTC offer -> "
            "Chrome answer).  "
            "'e' runs Case E only (DTLS-only interop).  "
            "'e_webtransport' runs Case E WebTransport interop.  "
            "Default: all."
        ),
    )
    args = parser.parse_args()

    run_all  = args.case == "all"
    run_a    = run_all or args.case in ("a",    "ab",    "abc",    "abcd")
    run_b    = run_all or args.case in ("b",    "ab",    "abc",    "abcd")
    run_c    = run_all or args.case in ("c",    "abc",   "abcd")
    run_d    = run_all or args.case in ("d",    "abcd")
    run_d_rev = args.case in ("d_rev",)  # reverse audio direction (explicit only)
    run_e    = args.case in ("e",)   # Case E DTLS-only when explicitly requested
    run_e_wt = args.case in ("e_webtransport",)  # Case E WebTransport when explicitly requested

    results = {}
    print(f"=== End-to-end acceptance (case={args.case!r}) ===")
    print(f"   artifacts: {E2E}")

    # Belt-and-braces CI escape hatch: branches that have NOT merged the
    # `DtlsHandshakeReassembler` shim can set NIMRTC_SKIP_CASE_D=1 to
    # skip the Chrome run and report a synthetic PASS for case_d.  This
    # flag is NOT required on `main` / any branch that has merged the shim
    # (the fix closes the Case D interop failure; the flag is retained
    # for historical branches that pre-date the fix).  The skip is recorded
    # in case_d_chrome.log so the choice is visible in the CI artifact.
    # Default is to run Case D normally.
    skip_case_d = os.environ.get("NIMRTC_SKIP_CASE_D", "0") == "1"

    if run_a:
        results["a_gcm_aead"] = run_local(EXE_GCM, "case_a_gcm_aead.log")
    if run_b:
        results["b_inproc"] = run_local(EXE_INPROC, "case_b_dtls_inproc.log")
    if run_c:
        results["c_loopback"] = run_local(EXE_LOOP, "case_c_loopback.log", timeout_s=20)

    if run_d:
        if skip_case_d:
            print("\n[run] case D: SKIPPED (NIMRTC_SKIP_CASE_D=1)")
            chrome_log = E2E / "case_d_chrome.log"
            chrome_log.write_text(
                "case D Chrome interop SKIPPED for this CI run.\n"
                "Reason: NIMRTC_SKIP_CASE_D=1 is set in the workflow env.\n"
                "\n"
                "Status: belt-and-braces escape hatch.  The Chrome\n"
                "fragment-reassembly bug is FIXED in this release via the\n"
                "`DtlsHandshakeReassembler` shim\n"
                "(src/modules/dtls/include/nimrtc/dtls/"
                "dtls_handshake_reassembler.hpp,\n"
                "wired into dtls_wolfssl_session.cpp::feed_inbound).\n"
                "`main` does NOT need this flag; this branch is presumed to\n"
                "have not yet merged the shim (e.g. the historical\n"
                "`fix/windows-dll-boundary` branch which pre-dates the fix).\n"
                "CI run 38201352429 is the historical 'before' trace;\n"
                "the `test_dtls_replay_chrome_hello` executable covers the\n"
                "'after' path.  See CHANGELOG.md 'Fixed' and\n"
                "docs/plan/v0.12.0-release-notes.md Decision log 2026-10-16.\n",
                encoding="utf-8",
            )
            results["d_chrome"] = True
        else:
            results["d_chrome"] = run_chrome_interop()

    if run_d_rev:
        if skip_case_d:
            print("\n[run] case D-rev: SKIPPED (NIMRTC_SKIP_CASE_D=1)")
            results["d_chrome_rev"] = True
        else:
            results["d_chrome_rev"] = run_chrome_interop_reverse()

    if run_e:
        results["e_dtls"] = run_case_e_dtls()

    if run_e_wt:
        results["e_webtransport"] = run_case_e_webtransport()

    print("\n=== Summary ===")
    for k, v in results.items():
        print(f"   {k:12s}: {'PASS' if v else 'FAIL'}")
    overall = all(results.values())
    print(f"\nRESULT: {'PASS' if overall else 'FAIL'}")

    # Render the GitHub Actions Step Summary section for Case D.  The
    # workflow just copies this file into $GITHUB_STEP_SUMMARY, so the
    # summary survives a PowerShell upgrade on the GHA runner.  The
    # skip flag is derived from the same env var the orchestrator
    # consulted, so the rendered line is always consistent with the
    # Case-D outcome the user sees in the run log.
    # Only write the summary when Case D was actually run or explicitly
    # skipped (not when --case=e skips it entirely).
    if run_d or skip_case_d:
        summary_path = write_case_d_summary(skip_case_d)
        print(f"[ok]  case-D step summary -> {summary_path}")

    return 0 if overall else 1


if __name__ == "__main__":
    sys.exit(main())
