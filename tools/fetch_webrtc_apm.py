#!/usr/bin/env python3
"""
Fetch and build WebRTC Audio Processing Module (APM) source.

This script clones the WebRTC APM source into:
    src/third_party/webrtc_audio_processing/src/

Then it either:
  - Windows: invokes tools/build_webrtc_apm.cmd
  - Linux/macOS: invokes tools/build_webrtc_apm.sh

WebRTC APM provides:
    - AEC:  Acoustic Echo Cancellation (desktop AEC3, mobile AECm)
    - ANS:  Ambient Noise Suppression (low/medium/high)
    - AGC:  Automatic Gain Control (fixed/digital/analog)
    - VAD:  Voice Activity Detection
    - Transient suppression
    - High-pass filter (80 Hz)

The upstream project uses Meson as its build system (not CMakeLists.txt).
On Windows, MSVC is required. On Linux/macOS, GCC 11+ or Clang 14+ is required.

Usage:
    python tools/fetch_webrtc_apm.py [--mirror freedesktop|google]

Options:
    --mirror    Which mirror to clone from:
                 freedesktop  - gitlab.freedesktop.org (default, works in China)
                 google       - chromium.googlesource.com (canonical source)
    --skip-build  Clone only, do not build (for CI caching)

Example:
    python tools/fetch_webrtc_apm.py                    # clone + build
    python tools/fetch_webrtc_apm.py --skip-build       # clone only
    python tools/fetch_webrtc_apm.py --mirror google    # use Google
"""

import argparse
import os
import shutil
import subprocess
import sys
import platform
from pathlib import Path

# Mirror list in priority order
MIRRORS = {
    "freedesktop": {
        "url": "https://gitlab.freedesktop.org/pulseaudio/webrtc-audio-processing.git",
        "branch": "master",
        "note": "PulseAudio-maintained fork; works in China; uses Meson + abseil-cpp",
    },
    "google": {
        "url": "https://chromium.googlesource.com/webrtc/modules/audio_processing",
        "branch": "webrtc-m129",
        "note": "Canonical Chromium source; may be firewalled in some regions",
    },
}


def get_project_root():
    return Path(__file__).parent.parent.resolve()


def get_source_dir(project_root):
    return project_root / "src" / "third_party" / "webrtc_audio_processing" / "src"


def run(cmd, cwd=None, check=True, timeout=300):
    """Run a command; return (rc, stdout)."""
    print(f"[CMD] {' '.join(cmd)}")
    try:
        result = subprocess.run(
            cmd, cwd=cwd, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, check=False, timeout=timeout,
        )
        ok = result.returncode == 0
        if ok:
            print(result.stdout[-1500:] if len(result.stdout) > 1500 else result.stdout)
        else:
            # CI-only failure modes (e.g. ninja exiting on a non-obvious
            # compile error inside a deeply-nested WebRTC APM translation
            # unit) need MUCH more context than the last 1000 chars — the
            # real diagnostic usually lives 50-200 lines earlier, hidden
            # behind `[STEP]` banners. Dump the last 8 KiB, and if the
            # underlying subprocess wrote a separate log file (the cmd
            # wrapper does, at $LOG_DIR\webrtc_apm_build.log), call it
            # out so the runner log makes it obvious.
            print(f"[FAIL rc={result.returncode}]")
            tail = result.stdout[-8192:] if len(result.stdout) > 8192 else result.stdout
            print(tail)
        return ok, result.stdout
    except subprocess.TimeoutExpired:
        print(f"[TIMEOUT after {timeout}s]")
        return False, ""
    except Exception as e:
        print(f"[EXCEPTION] {e}")
        return False, ""


def test_connectivity(url):
    """Return True if the URL is reachable (ls-remote works)."""
    rc, _ = run(["git", "ls-remote", "--heads", url], timeout=20, check=False)
    return rc


def clone_source(source_dir, mirror_url, branch):
    """Clone the mirror into source_dir. Return True on success."""
    if source_dir.exists():
        if (source_dir / "meson.build").exists():
            print(f"[INFO] Source already present at {source_dir}")
            return True
        else:
            print(f"[INFO] Partial source dir found, removing and re-cloning...")
            shutil.rmtree(source_dir)

    source_dir.parent.mkdir(parents=True, exist_ok=True)

    ok, _ = run([
        "git", "clone",
        "--depth", "1",
        "--branch", branch,
        "--single-branch",
        mirror_url,
        str(source_dir),
    ], timeout=120)

    if not ok:
        # Try shallow but with history (depth=50) for submodules
        ok, _ = run([
            "git", "clone",
            "--depth", "50",
            "--branch", branch,
            "--single-branch",
            mirror_url,
            str(source_dir),
        ], timeout=120)

    return ok


def find_build_script(project_root):
    """Locate the appropriate build script for the current OS."""
    if platform.system() == "Windows":
        script = project_root / "tools" / "build_webrtc_apm.cmd"
        if script.exists():
            return script, "cmd.exe"
    else:
        script = project_root / "tools" / "build_webrtc_apm.sh"
        if script.exists():
            return script, "bash"
    # Legacy fallback: a copy may still live under build/ (gitignored, not
    # tracked). Prefer tools/ but accept build/ for older checkouts.
    if platform.system() == "Windows":
        legacy = project_root / "build" / "build_webrtc_apm.cmd"
    else:
        legacy = project_root / "build" / "build_webrtc_apm.sh"
    if legacy.exists():
        print(f"[INFO] Using legacy script at {legacy}; consider moving it to tools/.")
        return legacy, "cmd.exe" if platform.system() == "Windows" else "bash"
    return None, None


def run_build_script(script_path, runner):
    """Run the platform-appropriate build script."""
    if runner == "cmd.exe":
        ok, _ = run(["cmd.exe", "/c", str(script_path)], timeout=1200)
    else:
        ok, _ = run(
            ["bash", str(script_path)],
            timeout=1200,
        )
    return ok


def main():
    parser = argparse.ArgumentParser(
        description="Fetch and build WebRTC Audio Processing Module.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "--mirror",
        default="freedesktop",
        choices=["freedesktop", "google"],
        help="Git mirror to clone from (default: freedesktop)",
    )
    parser.add_argument(
        "--skip-build",
        action="store_true",
        help="Clone only; do not run the build step",
    )
    args = parser.parse_args()

    project_root = get_project_root()
    source_dir = get_source_dir(project_root)

    print("=" * 60)
    print("WebRTC APM Fetch & Build")
    print("=" * 60)
    print(f"Project root : {project_root}")
    print(f"Source dir  : {source_dir}")
    print(f"Mirror      : {args.mirror} ({MIRRORS[args.mirror]['note']})")
    print(f"URL         : {MIRRORS[args.mirror]['url']}")
    print(f"Branch      : {MIRRORS[args.mirror]['branch']}")
    print(f"Skip build  : {args.skip_build}")
    print()

    # Step 1: connectivity test
    print("[STEP 0] Testing mirror connectivity...")
    mirror_info = MIRRORS[args.mirror]
    if test_connectivity(mirror_info["url"]):
        print(f"  -> {args.mirror} is reachable.")
    else:
        print(f"  -> {args.mirror} is NOT reachable.")
        print("  -> Falling back to other mirrors...")
        for name, info in MIRRORS.items():
            if name == args.mirror:
                continue
            print(f"  -> Testing {name}: {info['url']}")
            if test_connectivity(info["url"]):
                mirror_info = info
                args.mirror = name
                print(f"  -> {name} works! Using it.")
                break
        else:
            print()
            print("=" * 60)
            print("[ERROR] No mirror is reachable.")
            print("=" * 60)
            print("Options:")
            print("  1. Check your network / proxy settings")
            print("  2. Try a different mirror: --mirror freedesktop")
            print("  3. Manually clone and build:")
            print("     git clone --depth=1 --branch=master \\")
            print(f"       {MIRRORS['freedesktop']['url']} \\")
            print(f"       {source_dir}")
            print()
            print("The audio3a module will continue with the built-in stub")
            print("implementation (no 3A processing) if this script fails.")
            return 1

    # Step 2: clone
    print()
    print(f"[STEP 1] Cloning from {args.mirror}...")
    if not clone_source(source_dir, mirror_info["url"], mirror_info["branch"]):
        print()
        print("[ERROR] Clone failed.")
        print("The audio3a module will fall back to the stub implementation.")
        return 1

    if not (source_dir / "meson.build").exists():
        print()
        print(f"[ERROR] meson.build not found after clone.")
        print(f"Contents of {source_dir}:")
        for p in sorted(source_dir.iterdir()):
            print(f"  {p.name}")
        return 1

    print(f"[OK] Source cloned to: {source_dir}")
    print()
    print(f"[OK] WebRTC APM source fetched successfully!")
    print()

    if args.skip_build:
        print("[INFO] --skip-build: stopping after clone.")
        print("  To build:")
        print("    Windows: tools\\build_webrtc_apm.cmd")
        print("    Linux:   bash tools/build_webrtc_apm.sh")
        return 0

    # Step 3: build
    print("[STEP 2] Running build script...")
    script_path, runner = find_build_script(project_root)
    if not script_path:
        print("[WARN] Build script not found.")
        print("  Expected:")
        print("    Windows: tools\\build_webrtc_apm.cmd")
        print("    Linux:   tools/build_webrtc_apm.sh")
        print()
        print("  Build manually:")
        print("    Windows: tools\\build_webrtc_apm.cmd")
        print("    Linux:   bash tools/build_webrtc_apm.sh")
        print()
        print("  Or configure NimRTC and it will auto-detect the pre-built library:")
        print(f"    cmake -B build -S {project_root}")
        return 0

    print(f"  -> Using: {script_path} (via {runner})")
    if not run_build_script(script_path, runner):
        print()
        print("[ERROR] Build failed. Check the output above.")
        print("The audio3a module will fall back to the stub implementation.")
        return 1

    print()
    print("=" * 60)
    print("[OK] WebRTC APM built successfully!")
    print("=" * 60)
    print()
    print("You can now build NimRTC with WebRTC APM support:")
    print(f"  cd {project_root}")
    print("  cmake -B build -S . -DNIMRTC_VENDORED_WEBRTC_APM=ON")
    print("  cmake --build build --config Release")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
