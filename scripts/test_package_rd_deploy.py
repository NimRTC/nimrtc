"""Self-test for scripts/package_rd_deploy.ps1.

The packaging script guards two failure modes that are both quiet and
both expensive:

1. **Mixing build trees.**  `msquic.dll` has to come from the same
   tree as the media binaries; a DLL from another configuration is an
   ABI mismatch that crashes on the operator's machine with nothing in
   the bundle to explain it.

2. **Shipping a stale binary.**  A bundle packaged before a fix landed
   ships the bug, and the operator sees the old behaviour and concludes
   the fix did not work.  The script has to *say* so.

Both are checked here against a throwaway tree, so a later change that
quietly drops either guard fails this test rather than the next bug
report.

Requires Windows PowerShell (the script under test is a .ps1).  Run:

    python scripts/test_package_rd_deploy.py
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "scripts" / "package_rd_deploy.ps1"

# The script insists on a real MSVC redistributable, which not every
# machine has.  The tests below pass an explicit -CrtDir, so the
# redistribution lookup is never reached -- but the *runtime* has to
# exist for the copy loop to succeed, so a stub DLL is enough.
STUB_CRT = ["msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll",
            "msvcp140_atomic_wait.dll", "msvcp140_codecvt_ids.dll",
            "vcruntime140.dll", "vcruntime140_1.dll", "vcruntime140_threads.dll",
            "concrt140.dll", "vccorlib140.dll"]


def powershell() -> str:
    """The PowerShell to run the script under."""
    for candidate in ("pwsh", "powershell"):
        if shutil.which(candidate):
            return candidate
    raise RuntimeError("no PowerShell on this machine")


def run_package(bin_dir: Path, out_dir: Path, crt_dir: Path,
                extra: list[str] | None = None) -> subprocess.CompletedProcess:
    """Run the packaging script and capture everything it said."""
    cmd = [
        powershell(), "-NoProfile", "-ExecutionPolicy", "Bypass",
        "-File", str(SCRIPT),
        "-BinDir", str(bin_dir),
        "-SrcDir", str(ROOT / "examples" / "remote-desktop"),
        "-CrtDir", str(crt_dir),
        "-OutDir", str(out_dir),
        "-ZipPath", str(out_dir.with_suffix(".zip")),
    ]
    cmd += extra or []
    return subprocess.run(cmd, capture_output=True, text=True)


def make_tree(root: Path, *, with_msquic: bool = True,
              binary_mtime: float | None = None) -> Path:
    """A build tree with the two media binaries (and optionally msquic)."""
    bin_dir = root / "tree"
    bin_dir.mkdir(parents=True, exist_ok=True)
    for exe in ("rd_device.exe", "rd_viewer.exe"):
        path = bin_dir / exe
        path.write_bytes(b"MZ fake binary")
        if binary_mtime is not None:
            os.utime(path, (binary_mtime, binary_mtime))
    if with_msquic:
        (bin_dir / "msquic.dll").write_bytes(b"MZ fake dll")
    # The DTLS certificate, which CMake stages next to the executables.
    # The bundle has to carry it: the operator's machine has no source
    # tree for the absolute fallback to find, and without it the
    # handshake fails with "wolfSSL_CTX setup failed".
    cert_dir = bin_dir / "certs"
    cert_dir.mkdir(parents=True, exist_ok=True)
    for pem in ("server-ecc.pem", "ecc-key.pem"):
        (cert_dir / pem).write_bytes(b"-----BEGIN CERTIFICATE-----\n")
    return bin_dir


def make_crt(root: Path) -> Path:
    """A stub redistributable directory with every DLL the script wants."""
    crt_dir = root / "crt"
    crt_dir.mkdir(parents=True, exist_ok=True)
    for dll in STUB_CRT:
        (crt_dir / dll).write_bytes(b"MZ fake dll")
    return crt_dir


def section(title: str) -> None:
    print(f"\n=== {title} ===")


def check(label: str, condition: bool, detail: str = "") -> bool:
    mark = "ok  " if condition else "FAIL"
    print(f"[{mark}] {label}" + (f" -- {detail}" if detail and not condition else ""))
    return condition


def main() -> int:
    failures = 0

    # ------------------------------------------------------------------
    section("a bundle from a good tree is assembled, and records its provenance")
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        bin_dir = make_tree(root)
        crt_dir = make_crt(root)
        out_dir = root / "out"
        proc = run_package(bin_dir, out_dir, crt_dir)
        failures += not check("the script succeeded", proc.returncode == 0,
                              proc.stdout + proc.stderr)
        for name in ("rd_device.exe", "rd_viewer.exe", "msquic.dll",
                     "client.py", "rd_viewer.bat", "BUNDLE-INFO.txt"):
            failures += not check(f"{name} is in the bundle",
                                  (out_dir / name).exists())
        info = (out_dir / "BUNDLE-INFO.txt").read_text(encoding="ascii",
                                                       errors="replace")
        failures += not check("the provenance names a commit",
                              "commit:" in info, info)
        failures += not check("the provenance says the bundle is fresh",
                              "stale:      no" in info, info)
        zip_path = out_dir.with_suffix(".zip")
        failures += not check("the archive was written", zip_path.exists())
        if zip_path.exists():
            with zipfile.ZipFile(zip_path) as zf:
                names = zf.namelist()
            failures += not check("the archive carries the media binary",
                                  any(n.endswith("rd_viewer.exe") for n in names),
                                  str(names[:10]))
            failures += not check("the archive carries the provenance",
                                  any(n.endswith("BUNDLE-INFO.txt") for n in names),
                                  str(names[:10]))

    # ------------------------------------------------------------------
    section("a tree without msquic.dll is refused, not mixed with another tree")
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        bin_dir = make_tree(root, with_msquic=False)
        crt_dir = make_crt(root)
        out_dir = root / "out"
        proc = run_package(bin_dir, out_dir, crt_dir)
        failures += not check("the script failed", proc.returncode != 0,
                              proc.stdout + proc.stderr)
        combined = (proc.stdout + proc.stderr).lower()
        failures += not check("the error names msquic.dll",
                              "msquic.dll" in combined, combined[:400])
        failures += not check("the error says one tree is required",
                              "one build tree" in combined, combined[:400])
        failures += not check("no bundle was written", not out_dir.exists())

    # ------------------------------------------------------------------
    section("binaries older than the sources are reported as stale")
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        # 2000-01-01: older than any source in the tree.
        bin_dir = make_tree(root, binary_mtime=946_684_800.0)
        crt_dir = make_crt(root)
        out_dir = root / "out"
        proc = run_package(bin_dir, out_dir, crt_dir)
        combined = proc.stdout + proc.stderr
        failures += not check("the warning was printed",
                              "OLDER than the sources" in combined,
                              combined[:600])
        failures += not check("the script still succeeded (it is a warning)",
                              proc.returncode == 0, combined[:400])
        info = (out_dir / "BUNDLE-INFO.txt").read_text(encoding="ascii",
                                                       errors="replace")
        failures += not check("the provenance records the staleness",
                              "stale:      YES" in info, info)

    # ------------------------------------------------------------------
    section("-NoStaleCheck silences the warning but keeps the record")
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        bin_dir = make_tree(root, binary_mtime=946_684_800.0)
        crt_dir = make_crt(root)
        out_dir = root / "out"
        proc = run_package(bin_dir, out_dir, crt_dir, ["-NoStaleCheck"])
        combined = proc.stdout + proc.stderr
        failures += not check("the warning is gone",
                              "OLDER than the sources" not in combined,
                              combined[:600])
        failures += not check("the script succeeded", proc.returncode == 0,
                              combined[:400])
        info = (out_dir / "BUNDLE-INFO.txt").read_text(encoding="ascii",
                                                       errors="replace")
        failures += not check("the provenance still records the staleness",
                              "stale:      YES" in info, info)

    # ------------------------------------------------------------------
    section("fresh binaries are not reported as stale")
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        # Far in the future: newer than any source in the tree.
        bin_dir = make_tree(root, binary_mtime=4_102_444_800.0)
        crt_dir = make_crt(root)
        out_dir = root / "out"
        proc = run_package(bin_dir, out_dir, crt_dir)
        combined = proc.stdout + proc.stderr
        failures += not check("no staleness warning",
                              "OLDER than the sources" not in combined,
                              combined[:600])
        info = (out_dir / "BUNDLE-INFO.txt").read_text(encoding="ascii",
                                                       errors="replace")
        failures += not check("the provenance says it is fresh",
                              "stale:      no" in info, info)

    print()
    if failures:
        print(f"FAILED: {failures} check(s) did not hold")
        return 1
    print("OK: the packaging script's guards hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
