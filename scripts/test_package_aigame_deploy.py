"""Self-test for scripts/package_aigame_deploy.ps1.

The AI-game bundle carries three things the remote-desktop bundle does
not, and each one is a quiet failure if it goes missing:

1. **The AI decision binaries.**  `demo-agent-realtime.exe` and
   `demo-agent-teammate.exe` are the "AI" in the "AI game demo".  A
   bundle that ships the media path without them is a bundle that
   cannot do the thing it is named after -- and nothing in it says so.
   The script has to *refuse* to build such a bundle.

2. **The game source.**  The device side has to capture the synthetic
   whack-a-mole board (`--game-source`), not the real screen: this
   bundle is for measuring remote-game latency, not for screen sharing.
   The `.bat` wrapper has to carry that flag.

3. **The latency report.**  A latency bundle that never prints a latency
   number is a latency bundle that measures nothing.  The viewer prints
   the QoE snapshot, and the README has to say what the numbers mean.

The provenance rules the remote-desktop bundle already has -- one build
tree, no stale binaries -- are checked here too, because this script
copied them and a copy that drifts is worse than no copy.

Requires Windows PowerShell (the script under test is a .ps1).  Run:

    python scripts/test_package_aigame_deploy.py
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
SCRIPT = ROOT / "scripts" / "package_aigame_deploy.ps1"

STUB_CRT = ["msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll",
            "msvcp140_atomic_wait.dll", "msvcp140_codecvt_ids.dll",
            "vcruntime140.dll", "vcruntime140_1.dll", "vcruntime140_threads.dll",
            "concrt140.dll", "vccorlib140.dll"]

AGENT_EXES = ("demo-agent-realtime.exe", "demo-agent-teammate.exe")


def powershell() -> str:
    """The PowerShell to run the script under."""
    for candidate in ("pwsh", "powershell"):
        if shutil.which(candidate):
            return candidate
    raise RuntimeError("no PowerShell on this machine")


def run_package(bin_dir: Path, out_dir: Path, crt_dir: Path,
                agent_bin_dir: Path | None = None,
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
    if agent_bin_dir is not None:
        cmd += ["-AgentBinDir", str(agent_bin_dir)]
    cmd += extra or []
    return subprocess.run(cmd, capture_output=True, text=True)


def make_tree(root: Path, *, with_msquic: bool = True,
              with_agents: bool = True,
              binary_mtime: float | None = None) -> tuple[Path, Path]:
    """A build tree with the media binaries (and optionally msquic).

    Returns the media bin dir and the agent bin dir.  The layout mirrors
    the real one: the media binaries sit in `examples/Release` and the
    agent binaries in `examples/demo-agent-realtime/Release`.
    """
    examples = root / "tree" / "examples"
    bin_dir = examples / "Release"
    bin_dir.mkdir(parents=True, exist_ok=True)
    for exe in ("rd_device.exe", "rd_viewer.exe"):
        path = bin_dir / exe
        path.write_bytes(b"MZ fake binary")
        if binary_mtime is not None:
            os.utime(path, (binary_mtime, binary_mtime))
    if with_msquic:
        (bin_dir / "msquic.dll").write_bytes(b"MZ fake dll")

    agent_bin_dir = examples / "demo-agent-realtime" / "Release"
    agent_bin_dir.mkdir(parents=True, exist_ok=True)
    if with_agents:
        for exe in AGENT_EXES:
            path = agent_bin_dir / exe
            path.write_bytes(b"MZ fake binary")
            if binary_mtime is not None:
                os.utime(path, (binary_mtime, binary_mtime))
    return bin_dir, agent_bin_dir


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
    section("a good tree yields a bundle that carries the AI demo")
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        bin_dir, agent_bin_dir = make_tree(root)
        crt_dir = make_crt(root)
        out_dir = root / "out"
        proc = run_package(bin_dir, out_dir, crt_dir, agent_bin_dir)
        failures += not check("the script succeeded", proc.returncode == 0,
                              proc.stdout + proc.stderr)

        for name in ("rd_device.exe", "rd_viewer.exe", "msquic.dll",
                     "client.py", "rd_viewer.bat", "rd_device.bat",
                     "BUNDLE-INFO.txt", "AIGAME-README.md"):
            failures += not check(f"{name} is in the bundle",
                                  (out_dir / name).exists())
        for exe in AGENT_EXES:
            failures += not check(f"{exe} is in the bundle",
                                  (out_dir / exe).exists())

        # ---- The AI decision binaries have their own entry points ----------
        for bat in ("demo_agent_self_test.bat", "demo_teammate_self_test.bat"):
            failures += not check(f"{bat} is in the bundle",
                                  (out_dir / bat).exists())

        # ---- The device captures the game, not the real screen -------------
        device_bat = (out_dir / "rd_device.bat").read_text(encoding="ascii",
                                                           errors="replace")
        failures += not check("the device wrapper asks for the game source",
                              "--game-source" in device_bat, device_bat)

        # ---- The provenance ------------------------------------------------
        info = (out_dir / "BUNDLE-INFO.txt").read_text(encoding="ascii",
                                                       errors="replace")
        failures += not check("the provenance names a commit",
                              "commit:" in info, info)
        failures += not check("the provenance names the agent binaries' tree",
                              "agents:" in info, info)
        failures += not check("the provenance says the bundle is fresh",
                              "stale:      no" in info, info)

        # ---- The README explains the latency numbers ------------------------
        readme = (out_dir / "AIGAME-README.md").read_text(encoding="utf-8",
                                                           errors="replace")
        failures += not check("the README documents the latency line",
                              "latency_ms" in readme, readme[:200])
        failures += not check("the README documents input-to-photon",
                              "input_to_photon" in readme, readme[:200])

        # ---- The archive ----------------------------------------------------
        zip_path = out_dir.with_suffix(".zip")
        failures += not check("the archive was written", zip_path.exists())
        if zip_path.exists():
            with zipfile.ZipFile(zip_path) as zf:
                names = zf.namelist()
            for exe in AGENT_EXES:
                failures += not check(
                    f"the archive carries {exe}",
                    any(n.endswith(exe) for n in names), str(names[:12]))
            failures += not check(
                "the archive carries the provenance",
                any(n.endswith("BUNDLE-INFO.txt") for n in names),
                str(names[:12]))

    # ------------------------------------------------------------------
    section("a bundle without the AI decision binaries is refused")
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        bin_dir, agent_bin_dir = make_tree(root, with_agents=False)
        crt_dir = make_crt(root)
        out_dir = root / "out"
        proc = run_package(bin_dir, out_dir, crt_dir, agent_bin_dir)
        failures += not check("the script refused", proc.returncode != 0,
                              proc.stdout + proc.stderr)
        failures += not check("the refusal names the missing binaries",
                              "demo-agent-realtime" in (proc.stdout + proc.stderr),
                              proc.stdout + proc.stderr)
        failures += not check("no bundle was left behind",
                              not (out_dir / "rd_viewer.exe").exists())

    # ------------------------------------------------------------------
    section("a tree without msquic.dll is refused, not mixed with another tree")
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        bin_dir, agent_bin_dir = make_tree(root, with_msquic=False)
        crt_dir = make_crt(root)
        out_dir = root / "out"
        proc = run_package(bin_dir, out_dir, crt_dir, agent_bin_dir)
        failures += not check("the script refused", proc.returncode != 0,
                              proc.stdout + proc.stderr)
        failures += not check("the refusal names msquic.dll",
                              "msquic" in (proc.stdout + proc.stderr),
                              proc.stdout + proc.stderr)

    # ------------------------------------------------------------------
    section("a stale binary is reported, not silently shipped")
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        # A binary older than every source in the tree.
        bin_dir, agent_bin_dir = make_tree(root, binary_mtime=1_000_000)
        crt_dir = make_crt(root)
        out_dir = root / "out"
        proc = run_package(bin_dir, out_dir, crt_dir, agent_bin_dir)
        failures += not check("the script warned about the stale binary",
                              "OLDER than the sources" in proc.stdout,
                              proc.stdout)
        info = (out_dir / "BUNDLE-INFO.txt").read_text(encoding="ascii",
                                                       errors="replace")
        failures += not check("the provenance records the staleness",
                              "stale:      YES" in info, info)

    # ------------------------------------------------------------------
    section("the agent binaries' tree is derived when it is not named")
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        bin_dir, agent_bin_dir = make_tree(root)
        crt_dir = make_crt(root)
        out_dir = root / "out"
        # No -AgentBinDir: the script has to derive it from -BinDir.
        proc = run_package(bin_dir, out_dir, crt_dir)
        failures += not check("the script succeeded", proc.returncode == 0,
                              proc.stdout + proc.stderr)
        for exe in AGENT_EXES:
            failures += not check(f"{exe} was found by derivation",
                                  (out_dir / exe).exists())

    print()
    if failures:
        print(f"FAIL: {failures} check(s) did not hold")
        return 1
    print("PASS: the AI-game bundle's guards all hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
