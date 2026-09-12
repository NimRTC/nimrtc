#!/usr/bin/env python3
"""
Check that all required tools and versions are present before building NimRTC.

Exit code: 0 = all OK, 1 = at least one check failed.

Usage:
    python tools/check_prerequisites.py          # all checks
    python tools/check_prerequisites.py --cmake  # cmake only
    python tools/check_prerequisites.py --compiler  # compiler only
    python tools/check_prerequisites.py --quiet  # minimal output
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import platform


# ---------------------------------------------------------------------------
# Minimum version requirements
# ---------------------------------------------------------------------------
REQUIRES = {
    "cmake": {
        "min": (3, 25),
        "name": "CMake",
        "url": "https://cmake.org/download/",
    },
    "python": {
        "min": (3, 8),
        "name": "Python",
        "url": "https://www.python.org/downloads/",
    },
    "msvc": {
        "min": (19, 43),
        "name": "MSVC",
        "url": "https://visualstudio.microsoft.com/downloads/",
        "label": "Visual Studio Build Tools >= 2022 (MSVC 19.43+)",
    },
    "gcc": {
        "min": (11, 0),
        "name": "GCC",
        "url": "https://gcc.gnu.org/",
        "label": "GCC >= 11",
    },
    "clang": {
        "min": (12, 0),
        "name": "Clang",
        "url": "https://releases.llvm.org/",
        "label": "Clang >= 12",
    },
    "apple_clang": {
        "min": (15, 0),
        "name": "Apple Clang",
        "url": "https://developer.apple.com/xcode/",
        "label": "Xcode >= 15 / Apple Clang >= 15",
    },
}


def _sym(ok: bool) -> str:
    """Return a check symbol that works across encodings (Windows GBK-friendly)."""
    return "[OK]" if ok else "[FAIL]"


def report(name: str, ok: bool, got: str, need: str, fix: str) -> bool:
    """Print a check result line and return its pass/fail status."""
    sym = _sym(ok)
    line = f"  {sym}  {name}: {got}" + ("" if ok else f"  (need {need})")
    print(line)
    if not ok:
        print(f"         {fix}")
    return ok


def run_cmd(cmd, cwd=None, timeout=30):
    try:
        r = subprocess.run(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, timeout=timeout, shell=isinstance(cmd, str),
            cwd=cwd,
        )
        return r.stdout.strip(), r.returncode
    except Exception as e:
        return str(e), 1


def parse_version(text: str) -> tuple:
    """Extract the first semver-like tuple from text, e.g. '13.2.0' -> (13,2,0)."""
    m = re.search(r"(\d+)\.(\d+)(?:\.(\d+))?", text)
    if not m:
        return (0,)
    return tuple(int(x) for x in m.groups())


def version_str(v: tuple) -> str:
    return ".".join(str(x) for x in v)


# ---------------------------------------------------------------------------
# Individual check functions
# ---------------------------------------------------------------------------

def check_cmake(verbose: bool) -> bool:
    """Check CMake >= 3.25."""
    r = REQUIRES["cmake"]
    cmake = shutil.which("cmake")
    if not cmake:
        return report(r["name"], False, "not found",
                      f">= {version_str(r['min'])}",
                      f"Download: {r['url']}")

    out, rc = run_cmd(["cmake", "--version"])
    if rc != 0:
        return report(r["name"], False, f"error: {out[:80]}",
                      f">= {version_str(r['min'])}",
                      f"Download: {r['url']}")

    ver = parse_version(out)
    ok = ver >= r["min"]
    return report(r["name"], ok, version_str(ver),
                  f">= {version_str(r['min'])}",
                  f"Download: {r['url']}" if not ok else "OK")


def check_python(verbose: bool) -> bool:
    """Check Python >= 3.8."""
    r = REQUIRES["python"]
    ver = sys.version_info[:2]
    ok = ver >= r["min"]
    return report(r["name"], ok, f"{ver[0]}.{ver[1]}",
                  f">= {version_str(r['min'])}",
                  f"Download: {r['url']}" if not ok else "OK")


def check_ninja(verbose: bool) -> bool:
    """Check Ninja (optional, but warn if missing on Windows)."""
    ninja = shutil.which("ninja")
    if ninja:
        out, _ = run_cmd(["ninja", "--version"])
        ver = parse_version(out)
        return report("Ninja", True, version_str(ver) if ver != (0,) else "found", "any", "OK")
    elif platform.system() == "Windows":
        return report("Ninja", False, "not found (required on Windows)",
                      "any",
                      "Install via:  choco install ninja  OR  scoop install ninja")
    else:
        return report("Ninja", True, "not found (optional on Linux/macOS)", "any", "OK -- using make")


def _msvc_version_from_vswhere() -> str:
    """Try to get installed MSVC toolset version via vswhere."""
    vswhere = os.path.join(
        os.environ.get("ProgramFiles(x86)", ""),
        "Microsoft Visual Studio", "Installer", "vswhere.exe"
    )
    if not os.path.exists(vswhere):
        # Try PATH
        vswhere = shutil.which("vswhere")

    if not vswhere:
        return ""

    out, _ = run_cmd([
        vswhere, "-latest", "-products", "*",
        "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
        "-property", "installationVersion"
    ])
    return out.strip()


def _msvc_clang_cl_version() -> tuple:
    """Try to read MSVC version from 'cl' compiler output."""
    cl = shutil.which("cl")
    if not cl:
        return (0,)
    out, _ = run_cmd(["cl"], timeout=10)
    # MSVC prints version in first line like "Microsoft (R) C/C++ ... Version 19.43.34433"
    m = re.search(r"Version\s+(\d+)\.(\d+)", out)
    if m:
        return (int(m.group(1)), int(m.group(2)))
    return (0,)


def check_msvc(verbose: bool) -> bool:
    """Check MSVC >= 19.43 (VS2022)."""
    if platform.system() != "Windows":
        return True  # skip

    r = REQUIRES["msvc"]
    cl = shutil.which("cl")
    if not cl:
        return report(r["name"], False, "cl.exe not found in PATH",
                      r["label"],
                      "Open 'x64 Native Tools Command Prompt for VS 2022' or install "
                      "Visual Studio Build Tools 2022: " + r["url"])

    # Try vswhere
    vs_ver = _msvc_version_from_vswhere()
    # Try cl --version
    ver = _msvc_clang_cl_version()
    if ver == (0,):
        return report(r["name"], False, "unknown version (could not parse cl output)",
                      r["label"],
                      "Install Visual Studio 2022 Build Tools: " + r["url"])

    ok = ver >= r["min"]
    return report(r["name"], ok, f"{ver[0]}.{ver[1]}",
                  r["label"],
                  "Install Visual Studio 2022 (17.4+) or update Build Tools: " + r["url"] if not ok else "OK")


def check_gcc(verbose: bool) -> bool:
    """Check GCC >= 11."""
    if platform.system() == "Windows":
        return True  # skip
    gcc = shutil.which("gcc")
    if not gcc:
        return report("GCC", False, "not found",
                      ">= 11",
                      "Install GCC 11+:  sudo apt install gcc-11 g++-11  (Ubuntu/Debian)")
    out, _ = run_cmd(["gcc", "-dumpfullversion"])
    ver = parse_version(out)
    if ver == (0,):
        out, _ = run_cmd(["gcc", "--version"])
        ver = parse_version(out)

    ok = ver >= (11, 0)
    return report("GCC", ok, version_str(ver) if ver != (0,) else "found",
                  ">= 11",
                  "Upgrade GCC:  sudo apt install gcc-11 g++-11  (Ubuntu/Debian)\n"
                  "              sudo dnf install gcc-toolset-11  (Fedora/RHEL 8+)\n"
                  "              See README 'Linux toolchain' section for more distros" if not ok else "OK")


def check_clang(verbose: bool) -> bool:
    """Check Clang >= 12."""
    if platform.system() == "Windows":
        return True
    clang = shutil.which("clang++")
    if not clang:
        clang = shutil.which("clang")
    if not clang:
        return report("Clang", False, "not found",
                      ">= 12",
                      "Install Clang 14+:  sudo apt install clang-14  (Ubuntu 22.04+)")
    out, _ = run_cmd([clang, "--version"])
    ver = parse_version(out)
    ok = ver >= (12, 0)
    return report("Clang", ok, version_str(ver) if ver != (0,) else "found",
                  ">= 12",
                  "Install Clang 14+:  sudo apt install clang-14  (Ubuntu 22.04+)" if not ok else "OK")


def check_apple_clang(verbose: bool) -> bool:
    """Check Apple Clang >= 15."""
    if platform.system() != "Darwin":
        return True
    r = REQUIRES["apple_clang"]
    out, _ = run_cmd(["clang++", "--version"])
    # Apple Clang uses version numbers independent of LLVM: "Apple clang version 15.0.0"
    m = re.search(r"Apple clang version\s+(\d+)", out)
    if not m:
        return report(r["name"], False, "unknown",
                      r["label"],
                      "Update Xcode: " + r["url"])
    ver = (int(m.group(1)), 0)
    ok = ver >= r["min"]
    return report(r["name"], ok, f"{ver[0]}.{ver[1]}",
                  r["label"],
                  "Update Xcode to >= 15: " + r["url"] if not ok else "OK")


def check_submodules(verbose: bool) -> bool:
    """Check that git submodules are initialized."""
    root = Path(__file__).parent.parent.resolve()
    submodule_file = root / ".gitmodules"
    if not submodule_file.exists():
        return report("Git submodules", True, "no .gitmodules found", "N/A", "OK")

    # Count initialized vs total
    out, _ = run_cmd(["git", "submodule", "status"], cwd=str(root), timeout=30)
    if "fatal" in out.lower() or "not a git" in out.lower():
        return report("Git submodules", False, "not a git repository",
                      "submodules present",
                      f"cd {root}  then  git submodule update --init --recursive")

    lines = [l for l in out.strip().splitlines() if l.strip()]
    missing = [l for l in lines if l.startswith("-")]
    if missing:
        return report("Git submodules", False,
                      f"{len(missing)} submodule(s) not initialized",
                      "all initialized",
                      f"cd {root}  then  git submodule update --init --recursive")
    return report("Git submodules", True, f"{len(lines)} submodule(s) initialized", "all OK", "OK")


def check_webrtc_apm(verbose: bool) -> bool:
    """Check if WebRTC APM source is present (warn if missing, it's optional)."""
    root = Path(__file__).parent.parent.resolve()
    apm_src = root / "src" / "third_party" / "webrtc_audio_processing" / "src"
    meson_build = apm_src / "meson.build"
    if meson_build.exists():
        lib_static = root / "build" / "_vendor_webrtc_apm_build" / "libwebrtc_audio_processing.a"
        status = "built" if lib_static.exists() else "source present (run build_webrtc_apm.cmd)"
        return report("WebRTC APM", True, status, "optional", "OK -- 3A support enabled")
    return report("WebRTC APM", True, "not found",
                  "optional",
                  "Optional:  python tools/fetch_webrtc_apm.py  -- enables AEC/ANS/AGC\n"
                  "           Or pass  -DNIMRTC_VENDORED_WEBRTC_APM=OFF  to skip 3A")


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(
        description="Check build prerequisites for NimRTC.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--cmake", action="store_true", help="Check CMake only")
    parser.add_argument("--compiler", action="store_true", help="Check compiler only")
    parser.add_argument("--quiet", "-q", action="store_true", help="Show only failures")
    args = parser.parse_args()

    checks = []

    sys_name = platform.system()
    print(f"NimRTC Prerequisites Check  [{sys_name}]")
    print()

    if args.cmake:
        checks = [check_cmake]
    elif args.compiler:
        checks = [
            check_msvc,
            check_gcc,
            check_clang,
            check_apple_clang,
            check_ninja,
        ]
    else:
        checks = [
            check_python,
            check_cmake,
            check_ninja,
            check_msvc,
            check_gcc,
            check_clang,
            check_apple_clang,
            check_submodules,
            check_webrtc_apm,
        ]

    results = [fn(verbose=not args.quiet) for fn in checks]
    passed = sum(results)
    total = len(results)

    print()
    print(f"Result: {passed}/{total} checks passed")

    if not all(results):
        print()
        print("Fix the failures above, then re-run this check.")
        print("Quick links:")
        print("  CMake:    https://cmake.org/download/")
        print("  VS2022:   https://visualstudio.microsoft.com/downloads/")
        print("  Ninja:    choco install ninja  OR  scoop install ninja")
        print()
        print("After fixing, build with:")
        print("  cmake --preset dev          # Linux/macOS")
        print("  cmake --preset dev.msvc     # Windows")
        return 1

    print()
    print("All prerequisites met. You can build:")
    if sys_name == "Windows":
        print("  cmake --preset dev.msvc")
        print("  cmake --build build --config Debug -j")
    else:
        print("  cmake --preset dev")
        print("  cmake --build build -j")
    return 0


if __name__ == "__main__":
    sys.exit(main())
