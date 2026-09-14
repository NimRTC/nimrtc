#!/usr/bin/env python3
# Copyright (c) 2026 NimRTC contributors.
# SPDX-License-Identifier: MIT
"""Cross-platform CI pre-flight sanity checks.

Run this before `cmake --preset` to catch missing tools early and surface
actionable diagnostics in CI logs.

Usage:
  python tools/ci_preflight.py                  # checks for current platform
  python tools/ci_preflight.py --preset debug    # verify a specific preset
  python tools/ci_preflight.py --preset debug.aarch64
  python tools/ci_preflight.py --list            # show platform + tool summary

Exit codes:
  0  all required tools present
  1  one or more tools missing
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import NamedTuple

# Force UTF-8 on stdout on Windows consoles whose default code page is GBK.
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

REPO_ROOT = Path(__file__).resolve().parent.parent
PRESETS_PATH = REPO_ROOT / "CMakePresets.json"

EXPECTED_CMAKE_VERSION_MIN = (3, 25)

# ---------------------------------------------------------------------------
# Platform detection
# ---------------------------------------------------------------------------

import platform as _platform

HOST_OS = _platform.system().lower()          # "windows", "linux", "darwin"
HOST_ARCH = _platform.machine().lower()       # "amd64", "arm64", "aarch64"


def cmake_version() -> tuple[int, int] | None:
    """Return (major, minor) of the system's cmake, or None if not found."""
    cmake = shutil.which("cmake")
    if not cmake:
        return None
    try:
        out = subprocess.run(
            ["cmake", "--version"],
            text=True,
            capture_output=True,
            timeout=15,
        )
        m = re.search(r"cmake version (\d+)\.(\d+)", out.stdout)
        if m:
            return (int(m.group(1)), int(m.group(2)))
    except Exception:
        pass
    return None


def tool_check(name: str) -> tuple[bool, str]:
    """Return (found, version_string) for a tool."""
    exe = shutil.which(name)
    if not exe:
        return False, "not found"
    # Short version string for known tools.
    if name == "cmake":
        v = cmake_version()
        return True, f"{v[0]}.{v[1]}" if v else "?"
    if name in ("gcc", "g++"):
        try:
            out = subprocess.run(
                [name, "--version"],
                text=True,
                capture_output=True,
                timeout=10,
            )
            first = out.stdout.splitlines()[0]
            return True, first
        except Exception:
            return True, exe
    if name in ("clang", "clang++"):
        try:
            out = subprocess.run(
                [name, "--version"],
                text=True,
                capture_output=True,
                timeout=10,
            )
            first = out.stdout.splitlines()[0]
            return True, first
        except Exception:
            return True, exe
    if name == "ninja":
        try:
            out = subprocess.run(
                ["ninja", "--version"],
                text=True,
                capture_output=True,
                timeout=10,
            )
            return True, out.stdout.strip()
        except Exception:
            return True, exe
    if name == "python3":
        v = sys.version_info
        return True, f"{v.major}.{v.minor}.{v.micro}"
    if name == "python":
        v = sys.version_info
        return True, f"{v.major}.{v.minor}.{v.micro}"
    return True, exe


# ---------------------------------------------------------------------------
# Preset validation
# ---------------------------------------------------------------------------

def load_presets() -> dict | None:
    if not PRESETS_PATH.exists():
        return None
    try:
        with PRESETS_PATH.open(encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


def preset_requires_toolchain(preset: dict) -> str | None:
    """Return the toolchain file path if set, else None."""
    return preset.get("toolchainFile") or None


def preset_condition(preset: dict) -> str:
    """Human-readable condition string."""
    c = preset.get("condition")
    if not c:
        return "always"
    t = c.get("type", "")
    lhs = c.get("lhs", "")
    rhs = c.get("rhs", "")
    op = c.get("operator", "==")
    if t == "equals":
        return f"{lhs} {op} {rhs}"
    return str(c)


def list_presets() -> int:
    presets = load_presets()
    if not presets:
        print("(CMakePresets.json not found)")
        return 0
    for p in presets.get("configurePresets", []):
        if p.get("hidden"):
            continue
        print(f"  {p['name']}: {p.get('displayName', '')}")
    return 0


# ---------------------------------------------------------------------------
# Per-platform required tools
# ---------------------------------------------------------------------------

# (tool_name, reason_for_requiring_it)
WINDOWS_REQUIRED = [
    ("cmake", "configure"),
    ("ninja", "fast build"),
    ("python", "test / tooling"),
]

LINUX_REQUIRED = [
    ("cmake", "configure"),
    ("ninja", "fast build"),
    ("python3", "test / tooling"),
    ("gcc", "C compiler"),
    ("g++", "C++ compiler"),
]

MACOS_REQUIRED = [
    ("cmake", "configure"),
    ("ninja", "fast build"),
    ("python3", "test / tooling"),
    ("clang", "C compiler"),
    ("clang++", "C++ compiler"),
]

PLATFORM_REQUIRED: dict[str, list[tuple[str, str]]] = {
    "windows": WINDOWS_REQUIRED,
    "linux": LINUX_REQUIRED,
    "darwin": MACOS_REQUIRED,
}


def check_platform(preset_name: str | None = None) -> int:
    """Run all checks for the current platform. Returns 0 on success."""
    errors: list[str] = []
    warnings: list[str] = []

    required = PLATFORM_REQUIRED.get(HOST_OS, LINUX_REQUIRED)

    print(f"[preflight] platform={HOST_OS} arch={HOST_ARCH}")
    print()

    # CMake version check
    v = cmake_version()
    if v is None:
        errors.append("cmake not found in PATH — install CMake ≥ 3.25")
    else:
        ok = v >= EXPECTED_CMAKE_VERSION_MIN
        status = "PASS" if ok else "FAIL"
        print(f"  cmake {v[0]}.{v[1]}         [cmake-version]  {status}")
        if not ok:
            errors.append(
                f"cmake {v[0]}.{v[1]} too old — need ≥ {EXPECTED_CMAKE_VERSION_MIN[0]}.{EXPECTED_CMAKE_VERSION_MIN[1]}"
            )

    # Tool checks
    print()
    for name, reason in required:
        found, version = tool_check(name)
        status = "FOUND" if found else "MISSING"
        label = "tool-check"
        print(f"  {name:<12} {version[:40]:<40} [{label}]  {status}")
        if not found:
            errors.append(f"{name} not found — required for {reason}")

    # Preset check
    print()
    if preset_name:
        presets = load_presets()
        if presets:
            match = next(
                (p for p in presets.get("configurePresets", []) if p["name"] == preset_name),
                None,
            )
            if match:
                print(f"  preset:       {preset_name}")
                print(f"  display:      {match.get('displayName', '')}")
                print(f"  condition:    {preset_condition(match)}")
                tc = preset_requires_toolchain(match)
                if tc:
                    print(f"  toolchain:    {tc}")
                else:
                    print(f"  toolchain:    (none)")
            else:
                errors.append(f"preset {preset_name!r} not found in CMakePresets.json")
        else:
            warnings.append("CMakePresets.json not found — cannot validate preset")

    print()
    if warnings:
        for w in warnings:
            print(f"  WARNING: {w}")
        print()

    if errors:
        print("PRE-FLIGHT FAILURES:")
        for e in errors:
            print(f"  ✗ {e}")
        print()
        print("Install the missing tool(s) and re-run.")
        return 1

    print("All pre-flight checks passed.")
    return 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description="CI pre-flight sanity checks.")
    parser.add_argument(
        "--preset",
        metavar="NAME",
        help="Also validate that CMakePresets.json contains this preset.",
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="List available (non-hidden) presets.",
    )
    args = parser.parse_args()

    if args.list:
        return list_presets()

    return check_platform(preset_name=args.preset)


if __name__ == "__main__":
    sys.exit(main())
