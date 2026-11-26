#!/usr/bin/env python3
"""Generate the Chrome-friendly DTLS certificate pair, portably.

Why this script exists
----------------------
The pair (`server-ecc.pem` + `ecc-key.pem`) is a **build artefact**, not a
checked-in file: it is produced by `scripts/build_dtls_cert.py` into
`tests/wolfssl_dtls/certs/`, which is gitignored (`.gitignore:227`).
A fresh checkout therefore has neither file, and CMake's
`nimrtc_copy_wolfssl_certs()` POST_BUILD rule copies them
**unconditionally** -- so a build without the pair dies with

    error MSB3073: "cmake.exe" -E copy_if_different
        .../tests/wolfssl_dtls/certs/ecc-key.pem .../certs/

It used to work in CI because the pair came from wolfSSL's own cert tree
(upstream tracks `certs/ecc-key.pem`), but the pair has moved out of the
vendored submodule -- a build artefact must not dirty a vendored
dependency's tracked files.

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
    python scripts/generate_dtls_certs_ci.py
    python scripts/generate_dtls_certs_ci.py --check-only
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


def run(cmd: list[str]) -> None:
    """Run a command, failing loudly with its output."""
    print(f"$ {' '.join(cmd)}")
    proc = subprocess.run(cmd, text=True, capture_output=True)
    if proc.stdout:
        print(proc.stdout.rstrip())
    if proc.stderr:
        print(proc.stderr.rstrip(), file=sys.stderr)
    if proc.returncode != 0:
        raise SystemExit(
            f"generate_dtls_certs_ci.py: `{cmd[0]}` exited {proc.returncode}"
        )


def pip_install(py: str) -> None:
    """Install `cryptography` with the platform's pip invocation."""
    if os.name == "nt":
        run([py, "-m", "pip", "install", "--upgrade", "cryptography<42"])
    else:
        # `--user` because the runners' Pythons are externally managed.
        run([py, "-m", "pip", "install", "--user", "--upgrade", "cryptography<42"])


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="generate_dtls_certs_ci.py")
    parser.add_argument(
        "--check-only",
        action="store_true",
        help="do not install or generate; just report whether the pair exists",
    )
    args = parser.parse_args(argv)

    if args.check_only:
        missing = [p for p in (CERT, KEY) if not p.exists()]
        for p in (CERT, KEY):
            state = "present" if p.exists() else "MISSING"
            print(f"[{state}] {p.relative_to(ROOT)}")
        if missing:
            print(
                "generate_dtls_certs_ci.py: the DTLS certificate pair is not "
                "generated -- the build would fail on the copy rule",
                file=sys.stderr,
            )
            return 1
        return 0

    py = interpreter()
    pip_install(py)
    run([py, str(ROOT / "scripts" / "build_dtls_cert.py")])

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
