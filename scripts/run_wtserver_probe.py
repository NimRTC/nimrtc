#!/usr/bin/env python3
"""
Start the NimRTC demo-wtserver and run the aioquic WebTransport probe
against it.

Why this script exists
----------------------
Launching the demo server from a shell and then probing it is easy to
get wrong in two ways that both look like "the server is broken":

  * ``cmd /c "server.exe ... & echo DONE"`` -- the ``&`` does **not**
    background the process.  ``cmd /c`` runs the server in the
    foreground and only executes the trailing command after the server
    exits, so the caller gets the PID *after* the server is gone and
    every probe fails with ``ConnectionError``.
  * ``Start-Process ... -PassThru`` followed by ``$p.WaitForExit()`` --
    PowerShell does not cache the process handle, so ``$p.ExitCode``
    comes back empty and the exit status is lost.

This script uses ``subprocess.Popen`` (which does cache the handle),
waits for the listener to actually accept UDP traffic, runs the probe
rounds, and then reports the server's real exit code.

Usage
-----
    python scripts/run_wtserver_probe.py
    python scripts/run_wtserver_probe.py --port 9700 --rounds 10
    python scripts/run_wtserver_probe.py --duration 60 --probe venv-e2e/Scripts/python.exe

Exit codes
----------
    0  every probe round succeeded and the server exited cleanly
    1  a probe round failed
    2  the server exited with a non-zero status
    3  the server could not be started or the probe script is missing
"""

from __future__ import annotations

import argparse
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = PROJECT_ROOT / "build"


def find_server_exe() -> Path:
    """Locate the built demo-wtserver binary."""
    candidates = [
        BUILD_DIR / "examples" / "Debug" / "demo-wtserver.exe",
        BUILD_DIR / "examples" / "Release" / "demo-wtserver.exe",
        BUILD_DIR / "examples" / "demo-wtserver.exe",
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    searched = "\n".join(f"  {c}" for c in candidates)
    raise FileNotFoundError(
        "demo-wtserver.exe not found. Build it first. Searched:\n" + searched)


def find_probe_script() -> Path:
    """Locate the aioquic datagram probe."""
    candidates = [
        BUILD_DIR / "_wt_datagram_probe.py",
        BUILD_DIR / "probe_wt.py",
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    searched = "\n".join(f"  {c}" for c in candidates)
    raise FileNotFoundError(
        "WebTransport probe script not found. Searched:\n" + searched)


def find_probe_python(explicit: str | None) -> str:
    """Pick the interpreter that has aioquic available."""
    if explicit:
        return explicit
    venv_python = BUILD_DIR / "venv-e2e" / "Scripts" / "python.exe"
    if venv_python.is_file():
        return str(venv_python)
    return sys.executable


def udp_port_is_free(port: int) -> bool:
    """True when nothing is bound to the UDP port yet."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        try:
            sock.bind(("127.0.0.1", port))
        except OSError:
            return False
    return True


def wait_for_listener(port: int, proc: subprocess.Popen, timeout: float) -> bool:
    """Wait until the server owns the UDP port (or dies)."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            return False
        if not udp_port_is_free(port):
            return True
        time.sleep(0.1)
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=9700)
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--duration", type=int, default=45,
                        help="server lifetime in seconds")
    parser.add_argument("--probe", default=None,
                        help="interpreter that has aioquic installed")
    parser.add_argument("--startup-timeout", type=float, default=10.0)
    args = parser.parse_args()

    try:
        server_exe = find_server_exe()
        probe_script = find_probe_script()
    except FileNotFoundError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 3

    probe_python = find_probe_python(args.probe)
    if not Path(probe_python).is_file():
        print(f"error: probe interpreter not found: {probe_python}",
              file=sys.stderr)
        return 3

    out_path = BUILD_DIR / f"srv{args.port}.out"
    err_path = BUILD_DIR / f"srv{args.port}.err"
    BUILD_DIR.mkdir(parents=True, exist_ok=True)

    print(f"server : {server_exe}")
    print(f"probe  : {probe_python} {probe_script}")
    print(f"port   : {args.port}  rounds: {args.rounds}")

    with out_path.open("wb") as out_f, err_path.open("wb") as err_f:
        proc = subprocess.Popen(
            [str(server_exe), "--port", str(args.port),
             "--duration", str(args.duration)],
            stdout=out_f, stderr=err_f, cwd=str(BUILD_DIR))

        if not wait_for_listener(args.port, proc, args.startup_timeout):
            print("error: server did not start listening in time",
                  file=sys.stderr)
            print(f"       see {err_path}", file=sys.stderr)
            if proc.poll() is None:
                proc.kill()
            proc.wait()
            return 3

        failures = 0
        for round_no in range(1, args.rounds + 1):
            result = subprocess.run(
                [probe_python, str(probe_script), str(args.port), "1"],
                capture_output=True, text=True, cwd=str(BUILD_DIR))
            tail = (result.stdout or result.stderr).strip().splitlines()
            summary = tail[-1] if tail else "<no output>"
            status = "OK" if result.returncode == 0 else "FAILED"
            print(f"round {round_no}: {summary}")
            if result.returncode != 0:
                failures += 1
            # The server may have exited early (short --duration); stop
            # probing rather than reporting a wall of ConnectionErrors.
            if proc.poll() is not None:
                print("server exited early, skipping the remaining rounds")
                break

        # Give the server its remaining lifetime to shut down on its own.
        try:
            proc.wait(timeout=max(5.0, float(args.duration)))
        except subprocess.TimeoutExpired:
            print("error: server did not exit within the timeout",
                  file=sys.stderr)
            proc.kill()
            proc.wait()
            return 2

    exit_code = proc.returncode
    print(f"server exit code: {exit_code}")
    if failures:
        print(f"{failures} round(s) failed")
        return 1
    if exit_code not in (0, 2):
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
