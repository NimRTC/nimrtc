#!/usr/bin/env python3
"""Drive the reference signalling service with two local clients.

This is the *cross-machine* shape, run on one machine: the device client
and the viewer client each run as their own process, connected to the
WebSocket signalling service over a real TCP socket, and the media
plane runs between the two as two separate processes that talk over real
UDP sockets through ICE.

The only difference from a genuine two-machine run is the loopback
address in the signalling URL — everything else (the WebSocket relay,
the ICE/DTLS/SRTP handshake, the capture/encode/decode path) is the
real thing.
"""

import argparse
import os
import platform
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
#: The signalling service and the client live in the example directory,
#: not next to this script.
EXAMPLE_DIR = os.path.abspath(os.path.join(HERE, "..", "examples", "remote-desktop"))


def start_service(port: int, host: str = "127.0.0.1"):
    return subprocess.Popen(
        [sys.executable, os.path.join(EXAMPLE_DIR, "signalling.py"),
         "--host", host, "--port", str(port)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )


def start_client(role: str, url: str, extra, log_path: str):
    cmd = [sys.executable, os.path.join(EXAMPLE_DIR, "client.py"),
           "--role", role, "--url", url] + extra
    # stdout and stderr both go to the log: the pairing code is printed
    # on stdout and the media binary's diagnostics on stderr.
    #
    # PYTHONUNBUFFERED matters: without it Python block-buffers stdout
    # when it is not a terminal, and the pairing code would sit in the
    # buffer until the process exited — the harness would never see it.
    env = dict(os.environ, PYTHONUNBUFFERED="1")
    log = open(log_path, "w", encoding="utf-8")
    return subprocess.Popen(cmd, stdout=log, stderr=log, text=True, env=env)


def read_pairing_code(log_path: str, timeout: float = 15.0) -> str:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with open(log_path, "r", encoding="utf-8") as fh:
                text = fh.read()
        except OSError:
            text = ""
        match = re.search(r"pairing code:\s*(\d{6})", text)
        if match:
            return match.group(1)
        time.sleep(0.3)
    raise SystemExit("the device never printed a pairing code")


def wait_for(proc, timeout: float) -> int:
    """Wait for a child, killing it if it overruns — and never raising.

    `subprocess.wait` raises `TimeoutExpired` when the child outlives the
    deadline, which used to abort the harness *before* the logs were
    dumped — and the logs are the whole point of a failing run.  A client
    that hangs (a device that never opens, a viewer stuck in a loop) is a
    result, not a crash: it is reported as a non-zero exit code so the
    verdict below still runs.
    """
    try:
        return proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.terminate()
        try:
            return proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            return -1


#: The marker the media binaries print when the session is over.  The
#: signalling clients keep running after this (the WebSocket stays open),
#: so the marker — not the client's exit code — is what ends the run.
MEDIA_END_MARKERS = ("state: closed", "engine open failed",
                     "error 0x", "the signalling channel closed")


def wait_for_media_end(proc, log_path: str, timeout: float) -> int:
    """Wait until the *media* session is over, then stop the client.

    Returns 0 when the media session ended normally, and a non-zero code
    when the client died early or the session never came up.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            # The client is gone.  It is a clean end only if the media
            # session had reached its end.
            return 0 if media_session_finished(log_path) else (proc.returncode or 1)
        if media_session_finished(log_path):
            return 0
        time.sleep(0.3)
    return wait_for(proc, 1)


def media_session_finished(log_path: str) -> bool:
    """Has the media session reached its end (or died) according to the log?"""
    try:
        with open(log_path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError:
        return False
    return any(marker in text for marker in MEDIA_END_MARKERS)


def read_viewer_stats(log_path: str):
    """Extract the last ``[rd_viewer] frames=… decoded=…`` statistics line.

    The viewer's periodic report is the only place the media plane's
    outcome is visible, so the harness parses it instead of guessing from
    the exit codes.
    """
    try:
        with open(log_path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError:
        return None

    pattern = re.compile(
        r"frames=(\d+)\s+decoded=(\d+)\s+rendered=(\d+)\s+dropped=(\d+)")
    matches = pattern.findall(text)
    if not matches:
        return None
    last = matches[-1]
    return {
        "frames": int(last[0]),
        "decoded": int(last[1]),
        "rendered": int(last[2]),
        "dropped": int(last[3]),
    }


# ---------------------------------------------------------------------------
# Chrome viewer (--viewer-url)
#
# When `--viewer-url <url>` is given, the Chrome waterfall page
# (interop/chrome/demo_waterfall.html) joins the run as a browser-side
# control group: it connects to a NimRTC WebTransport server with
# `new WebTransport(url)`, draws the received-frame waterfall, and is
# handed the run's verdict on its verdict channel when the session ends.
#
# The verdict channel is the page's `window._rdVerdict` /
# `window._showVerdict` hook (driven here over the browser channel via
# Playwright); the page also accepts the same JSON object over the
# WebTransport data plane.  A best-effort `demo-wtserver` is started on
# --wt-port so the page has a live WebTransport plane to draw.
#
# When `--viewer-url` is NOT passed, none of this runs and the harness
# behaves exactly as before.
# ---------------------------------------------------------------------------

_CHROME_CANDIDATES = {
    "Windows": [
        r"C:\Program Files\Google\Chrome\Application\chrome.exe",
        r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
    ],
    "Linux": [
        "/usr/bin/google-chrome",
        "/usr/bin/google-chrome-stable",
        "/usr/bin/chromium",
        "/usr/bin/chromium-browser",
        "/snap/bin/chromium",
    ],
    "Darwin": [
        "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
        "/Applications/Chromium.app/Contents/MacOS/Chromium",
    ],
}


def _find_chrome() -> str:
    """Locate a Chrome/Chromium executable, or return an empty string.

    ``NIMRTC_CHROME_PATH`` wins; otherwise the per-platform candidate
    list is tried.  An empty result tells Playwright to fall back to its
    bundled Chromium.
    """
    override = os.environ.get("NIMRTC_CHROME_PATH", "").strip()
    if override and os.path.isfile(override):
        return override
    for candidate in _CHROME_CANDIDATES.get(platform.system(), []):
        if os.path.isfile(candidate):
            return candidate
    return ""


def find_demo_wtserver() -> str:
    """Locate the demo-wtserver binary in the build tree (best-effort).

    The waterfall page's own data plane needs a WebTransport listener to
    light up; when the binary has not been built we carry on without it —
    the page reports its own connection state honestly.
    """
    exe = ".exe" if os.name == "nt" else ""
    build_root = os.path.abspath(os.path.join(HERE, "..", "build"))
    candidates = []
    if os.path.isdir(build_root):
        for tree in sorted(os.listdir(build_root)):
            base = os.path.join(build_root, tree)
            if not os.path.isdir(base):
                continue
            candidates += [
                os.path.join(base, "examples", "demo-wtserver" + exe),
                os.path.join(base, "examples", "demo-wtserver",
                             "demo-wtserver" + exe),
                os.path.join(base, "Debug", "examples", "demo-wtserver" + exe),
                os.path.join(base, "Release", "examples", "demo-wtserver" + exe),
            ]
    found = [p for p in candidates if os.path.isfile(p)]
    return max(found, key=os.path.getmtime) if found else ""


def launch_chrome_viewer(viewer_url: str, wt_port: int, timeout_s: int):
    """Open the Chrome waterfall page; return (page, browser) or (None, None).

    Best-effort on every leg: a missing demo-wtserver binary, a missing
    Playwright, or a missing Chrome are all reported and degrade to
    (None, None) — the console verdict still prints, and the run is
    marked FAIL because the requested viewer was not reachable.
    """
    wtserver = find_demo_wtserver()
    wtserver_proc = None
    if wtserver:
        env = dict(os.environ,
                   NIMRTC_BUILD_ROOT=os.path.abspath(os.path.join(HERE, "..", "build")))
        try:
            wtserver_proc = subprocess.Popen(
                [wtserver, "--port", str(wt_port),
                 "--duration", str(timeout_s + 30)],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env,
            )
            print(f"[viewer] demo-wtserver on UDP :{wt_port} for the waterfall page")
        except OSError as exc:
            print(f"[viewer] could not start demo-wtserver: {exc}")
    else:
        print("[viewer] demo-wtserver not built — the page will report "
              "its own connection state")

    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        print("[viewer] playwright is not installed — cannot drive the Chrome "
              "page (pip install playwright && python -m playwright install chromium)")
        if wtserver_proc is not None:
            wtserver_proc.terminate()
        return None, None

    chrome_exe = _find_chrome()
    launch_kwargs = {"headless": True}
    if chrome_exe:
        launch_kwargs["executable_path"] = chrome_exe
    try:
        pw = sync_playwright().start()
        browser = pw.chromium.launch(
            args=["--webtransport-developer-mode", "--ignore-certificate-errors"],
            **launch_kwargs,
        )
        context = browser.new_context()
        page = context.new_page()
        page.goto(viewer_url)
        print(f"[viewer] Chrome waterfall page opened: {viewer_url}")
        return page, browser
    except Exception as exc:
        print(f"[viewer] could not launch the Chrome page: {exc}")
        try:
            pw.stop()
        except Exception:
            pass
        if wtserver_proc is not None:
            wtserver_proc.terminate()
        return None, None


def deliver_verdict_to_viewer(page, verdict: dict) -> None:
    """Push the run verdict onto the Chrome page's verdict channel.

    The page reads `window._rdVerdict` and renders it through
    `window._showVerdict(verdict)`; it also accepts the same object over
    the WebTransport data plane (see interop/chrome/demo_waterfall.html).
    """
    try:
        page.evaluate(
            "(v) => { window._rdVerdict = v;"
            " if (typeof window._showVerdict === 'function')"
            "   window._showVerdict(v); }",
            verdict,
        )
        print("[viewer] verdict delivered to the Chrome page ✓")
    except Exception as exc:
        print(f"[viewer] could not deliver the verdict to the Chrome page: {exc}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--media", default="rd",
                        help="demo-p2p | rd")
    parser.add_argument("--duration", type=int, default=10)
    parser.add_argument("--port", type=int, default=8766)
    parser.add_argument("--binary", default=None)
    parser.add_argument("--inject", action="store_true")
    parser.add_argument("--sink", default=None)
    parser.add_argument("--capture", default=None)
    parser.add_argument("--backend", default=None,
                        help="the codec backend: auto | hardware | software")
    parser.add_argument("--no-stun", action="store_true",
                        help="gather host candidates only (the LAN-only mode: "
                             "no server-reflexive candidate from a public "
                             "STUN server)")
    parser.add_argument("--bind", default="127.0.0.1",
                        help="the address the signalling service binds to, and "
                             "the address the clients connect to. Use the "
                             "LAN address (e.g. 192.168.1.20) to exercise the "
                             "cross-network shape instead of the loopback")
    parser.add_argument("--viewer-url", default=None,
                        help="optional: URL of the Chrome waterfall page "
                             "(interop/chrome/demo_waterfall.html) to drive as "
                             "a browser-side control group. The run verdict is "
                             "pushed onto the page's verdict channel when the "
                             "session ends. When omitted the harness behaves "
                             "exactly as before (device + viewer clients only).")
    parser.add_argument("--wt-port", type=int, default=9443,
                        help="WebTransport UDP port for the Chrome viewer page "
                             "(default 9443)")
    args = parser.parse_args()

    # HERE is <repo>/tools, so the repository root is one level up and
    # the build tree is a sibling of it.
    log_dir = os.path.abspath(os.path.join(HERE, "..", "build"))
    os.makedirs(log_dir, exist_ok=True)
    device_log = os.path.join(log_dir, "lan_device.log")
    viewer_log = os.path.join(log_dir, "lan_viewer.log")

    url = f"ws://{args.bind}:{args.port}"
    service = start_service(args.port, args.bind)
    time.sleep(1.5)

    device_extra = ["--media", args.media, "--duration", str(args.duration)]
    if args.no_stun:
        device_extra.append("--no-stun")
    if args.binary:
        device_extra += ["--binary", args.binary]
    if args.capture:
        device_extra += ["--capture", args.capture]
    if args.backend:
        device_extra += ["--backend", args.backend]
    if args.inject:
        device_extra.append("--inject")

    device = start_client("device", url, device_extra, device_log)
    viewer_page = None
    viewer_browser = None
    verdict_delivered = False
    try:
        code = read_pairing_code(device_log)

        # ---- Chrome viewer (--viewer-url) ---------------------------------
        # Open the waterfall page as a browser-side control group: it
        # connects to a WebTransport listener with `new WebTransport(url)`
        # and is handed the run verdict on its verdict channel when the
        # media session ends.  Without --viewer-url this block is skipped
        # entirely — behaviour is exactly as before.
        if args.viewer_url:
            viewer_page, viewer_browser = launch_chrome_viewer(
                args.viewer_url, args.wt_port, args.duration + 30)
            if viewer_page is None:
                print("FAIL: --viewer-url given but the Chrome viewer could "
                      "not be launched — see the [viewer] notes above")

        viewer_extra = ["--media", args.media, "--duration", str(args.duration),
                        "--code", code]
        if args.no_stun:
            viewer_extra.append("--no-stun")
        if args.binary:
            viewer_extra += ["--binary", args.binary]
        if args.sink:
            viewer_extra += ["--sink", args.sink]
        if args.backend:
            viewer_extra += ["--backend", args.backend]

        viewer = start_client("viewer", url, viewer_extra, viewer_log)

        # The *media* session has its own lifetime (--duration) and it is
        # the thing under test.  The signalling clients outlive it: they
        # keep the WebSocket open until the peer disappears, so waiting for
        # the client processes to exit would only measure the harness's
        # own patience.
        #
        # So the run ends when the media session is over — either because
        # the duration elapsed (both binaries print their final state
        # line) or because a client died early.
        device_rc = wait_for_media_end(device, device_log,
                                       args.duration + 25)
        viewer_rc = wait_for_media_end(viewer, viewer_log, 25)
    finally:
        service.terminate()
        try:
            service.wait(timeout=5)
        except subprocess.TimeoutExpired:
            service.kill()
        # If an exception unwound the try before the verdict was delivered,
        # close the Chrome viewer so nothing is left running.
        if viewer_browser is not None and not verdict_delivered:
            try:
                viewer_browser.close()
            except Exception:
                pass

    # The clients are terminated on the way out; that is expected and is
    # not a failure of the session.
    for proc in (device, viewer):
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()

    # ---- The verdict ------------------------------------------------------
    #
    # The exit codes alone are not a verdict: the device and the viewer
    # both exit cleanly even when the media plane never produced a
    # picture.  What the run is *for* is the media plane, so the harness
    # asserts on the decoded-frame counters.
    stats = read_viewer_stats(viewer_log)

    print("=" * 70)
    print("--- device log ---")
    with open(device_log, "r", encoding="utf-8") as fh:
        print(fh.read()[-4000:])
    print("--- viewer log ---")
    with open(viewer_log, "r", encoding="utf-8") as fh:
        print(fh.read()[-4000:])
    print("=" * 70)
    print(f"rc device={device_rc} viewer={viewer_rc}")

    ok = device_rc == 0 and viewer_rc == 0
    if stats is None:
        print("FAIL: the viewer never reported a frame statistics line")
        ok = False
    else:
        decoded = stats.get("decoded", 0)
        rendered = stats.get("rendered", 0)
        dropped = stats.get("dropped", 0)
        print(f"media plane: decoded={decoded} rendered={rendered} dropped={dropped}")
        if decoded <= 0:
            print("FAIL: no frame was decoded — the media plane did not come up")
            ok = False
        elif rendered <= 0:
            print("FAIL: frames were decoded but none were rendered")
            ok = False
        else:
            print("OK: the media plane delivered and rendered frames")

    # ---- Chrome verdict delivery ------------------------------------------
    # The verdict goes onto the Chrome page's verdict channel so the
    # browser-side control group shows the same PASS/FAIL as the console.
    if args.viewer_url:
        if viewer_page is not None:
            verdict_obj = {
                "pass": ok,
                "decoded": stats.get("decoded") if stats is not None else None,
                "rendered": stats.get("rendered") if stats is not None else None,
                "dropped": stats.get("dropped") if stats is not None else None,
                "frames": stats.get("frames") if stats is not None else None,
            }
            deliver_verdict_to_viewer(viewer_page, verdict_obj)
            verdict_delivered = True
            try:
                viewer_browser.close()
            except Exception:
                pass
        else:
            print("FAIL: the Chrome viewer was requested but never opened — "
                  "the verdict was not delivered to the page")
            ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
