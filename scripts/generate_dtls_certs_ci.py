#!/usr/bin/env python3
"""Generate the Chrome-friendly DTLS certificate pair, portably.

Why this script exists
----------------------
The pair (`server-ecc.pem` + `ecc-key.pem`) is a **build artefact**, not a
checked-in file: it is produced by `scripts/build_dtls_cert.py` into
`tests/wolfssl_dtls/certs/`, which is gitignored (`.gitignore:227`).

Since 2026-11-27 the build generates it itself:
`nimrtc_generate_dtls_certs()` in `cmake/NimRTCRuntime.cmake` declares a
single `nimrtc_dtls_certs` target, and the staging helpers
(`nimrtc_copy_wolfssl_certs()`, `nimrtc_stage_dtls_certs()`) depend on
it.  A workflow therefore does **not** need a "generate the certs" step
any more -- and the fifteen hand-written copies that used to carry one
(ci 9, soak 2, interop 2, ci-fast 1, hardware 1) are gone.

This script remains for the two cases CMake cannot cover:

  * **`--check-only`** -- the guard workflows call instead of
    generating.  It answers "is the pair present and current?" from the
    build's own verdict, with **no pip install**, so a job that is
    about to fail anyway does not spend a minute on the network first.
    Crucially it runs the *same* interpreter the CMake configure found,
    so its answer and the build's answer cannot disagree.
  * **a host that runs tests outside CMake** (a scratch build of one
    module, a Windows box without a configure) -- `--check-only` says
    what is missing, and running it without `--check-only` installs
    and generates.

The platform split
------------------
`scripts/build_dtls_cert.py` needs the `cryptography` package, and the
install line differs per platform:

  * **Windows** -- `py -m pip install` (or `python -m pip install`); the
    runners ship `python` on PATH.
  * **Linux / macOS** -- `python3 -m pip install --user`; `python3` is
    the interpreter name, and `--user` is needed because the runners'
    Pythons are externally managed (PEP 668).

`cryptography<42` on purpose: 42+ ships a Rust-backed Windows wheel
whose `cryptography_rust.dll` is not bundled in the PyPI artefact, so
`import cryptography` fails with "DLL load failed while importing
_rust".  41.x is the last pure-C line.

Usage:
    python scripts/generate_dtls_certs_ci.py --check-only
    python scripts/generate_dtls_certs_ci.py
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CERT_DIR = ROOT / "tests" / "wolfssl_dtls" / "certs"
CERT = CERT_DIR / "server-ecc.pem"
KEY = CERT_DIR / "ecc-key.pem"

# The interpreter to try, in order, for the current platform.
WINDOWS_PYTHONS = ("python", "py")
POSIX_PYTHONS = ("python3", "python")


def interpreter() -> str:
    """The Python to use, or an error naming what was looked for."""
    candidates = WINDOWS_PYTHONS if os.name == "nt" else POSIX_PYTHONS
    for name in candidates:
        path = shutil.which(name)
        if path:
            return path
    raise SystemExit(
        "generate_dtls_certs_ci.py: no Python interpreter found "
        f"(tried {', '.join(candidates)})"
    )


def run(cmd: list[str]) -> subprocess.CompletedProcess:
    """Run a command, echoing its output, and return the result."""
    print(f"$ {' '.join(cmd)}")
    proc = subprocess.run(cmd, text=True, capture_output=True,
                          encoding="utf-8", errors="replace")
    if proc.stdout:
        print(proc.stdout.rstrip())
    if proc.stderr:
        print(proc.stderr.rstrip(), file=sys.stderr)
    return proc


def pip_install(py: str) -> None:
    """Install `cryptography` with the platform's pip invocation."""
    if os.name == "nt":
        run([py, "-m", "pip", "install", "--upgrade", "cryptography<42"])
    else:
        # `--user` because the runners' Pythons are externally managed.
        run([py, "-m", "pip", "install", "--user", "--upgrade",
             "cryptography<42"])


# The verdict codes `build_dtls_cert.verify_pair()` returns.
CURRENT, MISSING, NO_SAN, STALE, NO_CRYPTO = 0, 1, 2, 3, 4


def ask_build() -> tuple[int, str]:
    """Run `verify_pair()` in `py`'s own site-packages, and return its answer.

    It has to be a child process: this script's own interpreter is
    whatever GitHub put on PATH, and the *build's* interpreter is the
    one CMake found -- and the two can have different site-packages.
    Asking the build's interpreter is what makes the answer true for
    the build.
    """
    py = interpreter()
    probe = (
        "import sys; sys.path.insert(0, %r)\n"
        "import build_dtls_cert as b\n"
        "code, reason = b.verify_pair()\n"
        "print(code)\n"
        "print(reason)\n"
    ) % str(ROOT / "scripts")
    proc = subprocess.run([py, "-c", probe], text=True, capture_output=True,
                          encoding="utf-8", errors="replace")
    if proc.returncode != 0:
        # The probe itself failed -- no interpreter, a broken
        # site-packages. Treat it as "cannot inspect", which is
        # NO_CRYPTO: the caller installs and lets the generator report.
        return NO_CRYPTO, (proc.stderr.strip().splitlines() or
                           ["the probe exited %d" % proc.returncode])[-1]
    lines = proc.stdout.splitlines()
    try:
        return int(lines[0]), lines[1] if len(lines) > 1 else ""
    except (IndexError, ValueError):
        return NO_CRYPTO, "the probe printed nothing parseable"


def report(code: int, reason: str) -> int:
    """Print the pair's files and the build's verdict."""
    for p in (CERT, KEY):
        state = "present" if p.exists() else "MISSING"
        print(f"[{state}] {p.relative_to(ROOT)}")
    verdict = {
        CURRENT: "current",
        MISSING: "MISSING",
        NO_SAN: "no valid SAN",
        STALE: "stale",
        NO_CRYPTO: "cannot be inspected",
    }.get(code, "unknown")
    print(f"[{verdict}] {reason}")
    return 0 if code == CURRENT else 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="generate_dtls_certs_ci.py")
    parser.add_argument(
        "--check-only",
        action="store_true",
        help="do not install or generate; report whether the pair is "
             "present and current (exit 0 when it is), and with the "
             "build's own interpreter"),
    args = parser.parse_args(argv)

    if args.check_only:
        code, reason = ask_build()
        rc = report(code, reason)
        if rc != 0:
            print(
                "generate_dtls_certs_ci.py: the DTLS certificate pair is not "
                "ready.  The CMake build generates it (`nimrtc_dtls_certs`), "
                "or run this script without --check-only.",
                file=sys.stderr,
            )
        return rc

    py = interpreter()

    # Install only when the pair is not already current.  A CI job that
    # restores a cached build tree (or ran a previous job in the same
    # workspace) does not need the network at all, and a job that does
    # need it still gets the *same* verdict the build will get.
    code, reason = ask_build()
    if code == CURRENT:
        print(f"[ok] the pair is current ({reason}); nothing to install")
        report(code, reason)
        return 0
    if code == NO_CRYPTO:
        pip_install(py)
    else:
        print(f"[regen] {reason}")

    proc = run([py, str(ROOT / "scripts" / "build_dtls_cert.py")])
    if proc.returncode != 0:
        raise SystemExit(
            f"generate_dtls_certs_ci.py: build_dtls_cert.py exited "
            f"{proc.returncode}")

    # The generator skips when the existing cert already covers every local
    # IP, so re-check rather than assume it wrote something.
    for p in (CERT, KEY):
        if not p.exists():
            raise SystemExit(
                f"generate_dtls_certs_ci.py: {p.relative_to(ROOT)} was not "
                "produced -- the build would fail on the copy rule"
            )
        print(f"[ok] {p.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
