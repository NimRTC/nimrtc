#!/usr/bin/env python3
# Copyright (c) 2026 NimRTC contributors.
# SPDX-License-Identifier: MIT
"""Measure NimRTC binary size and report embeddability numbers.

Run AFTER `cmake --build build` to get concrete numbers for the README
"Benchmarks" section.

Outputs (human-readable by default, --json for machines):
  - Static library size (libnimrtc_engine.a / nimrtc_engine.lib)
  - Built example binary size (loopback-p2p, demo-p2p)
  - Estimated total footprint if statically linked into a host app
  - Comparison vs known public libwebrtc reference numbers

Usage:
  python tools/benchmark_size.py                 # auto-detect build dir
  python tools/benchmark_size.py build/release   # explicit build dir
  python tools/benchmark_size.py --json          # JSON output
  python tools/benchmark_size.py --markdown      # ready-to-paste README table

Reference numbers (libwebrtc) are sourced from:
  - libwebrtc static library: 150-250 MB (varies by platform, level, deps)
  - libwebrtc monolithic .a/.lib with all codecs: 200+ MB typical
  - webrtc.org native-code size guide (public, well-known)

NOTE: NimRTC numbers are measured live. libwebrtc numbers are published
references and should be re-validated before publishing if they get stale.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import subprocess
import sys
from pathlib import Path
from typing import Optional

# ---------------------------------------------------------------------------
# Reference numbers for libwebrtc (well-known public references).
# These are approximate and platform-dependent. Update if you re-validate.
# ---------------------------------------------------------------------------
LIBWEBRTC_REFERENCE = {
    "static_lib_min_mb": 150,
    "static_lib_max_mb": 250,
    "monolithic_with_codecs_mb": 200,
    "source_locale_count": 5500_000,  # ~5.5M LoC, well-cited
    "minimal_peerconnection_estimate_mb": 100,
}


def find_files(root: Path, pattern: str) -> list[Path]:
    """Recursively find files matching a glob pattern under root."""
    return sorted(root.glob(pattern))


def humanize_bytes(n: int) -> str:
    """Convert bytes to a human string."""
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024:
            return f"{n:.2f} {unit}" if unit != "B" else f"{n} B"
        n /= 1024  # type: ignore[assignment]
    return f"{n:.2f} TB"


def locate_static_lib(build_dir: Path) -> Optional[Path]:
    """Find the engine static library across platform conventions."""
    patterns = []
    if platform.system() == "Windows":
        patterns = [
            "**/Release/nimrtc_engine.lib",
            "**/Debug/nimrtc_engine.lib",
            "**/nimrtc_engine.lib",
        ]
    elif platform.system() == "Darwin":
        patterns = ["**/libnimrtc_engine.a"]
    else:
        patterns = ["**/libnimrtc_engine.a"]

    for pat in patterns:
        hits = find_files(build_dir, pat)
        if hits:
            return hits[0]
    return None


def locate_examples(build_dir: Path) -> list[tuple[str, Path]]:
    """Find built example binaries."""
    results: list[tuple[str, Path]] = []
    if platform.system() == "Windows":
        candidates = [
            ("loopback-p2p", "**/Release/loopback-p2p.exe"),
            ("loopback-p2p", "**/Debug/loopback-p2p.exe"),
            ("demo-p2p", "**/Release/demo-p2p.exe"),
            ("demo-p2p", "**/Debug/demo-p2p.exe"),
        ]
    else:
        candidates = [
            ("loopback-p2p", "**/loopback-p2p"),
            ("demo-p2p", "**/demo-p2p"),
        ]

    for name, pat in candidates:
        for p in find_files(build_dir, pat):
            if p.is_file():
                results.append((name, p))
                break  # first hit per (name, pat) is enough
    return results


def measure(build_dir: Path) -> dict:
    """Run the actual measurements."""
    result: dict = {
        "build_dir": str(build_dir),
        "platform": platform.platform(),
        "static_lib": None,
        "examples": [],
    }

    static = locate_static_lib(build_dir)
    if static:
        result["static_lib"] = {
            "path": str(static),
            "size_bytes": static.stat().st_size,
            "size_human": humanize_bytes(static.stat().st_size),
        }

    for name, path in locate_examples(build_dir):
        result["examples"].append(
            {
                "name": name,
                "path": str(path),
                "size_bytes": path.stat().st_size,
                "size_human": humanize_bytes(path.stat().st_size),
            }
        )

    # Estimate NimRTC footprint when statically linked into a host app
    # = static lib + typical vendored deps (libsrtp, mbedtls, opus, libjuice)
    if result["static_lib"]:
        # Vendored deps sizes (approximate, build-time measured would be better)
        vendored_deps_estimate_mb = {
            "libsrtp": 0.4,
            "wolfssl": 3.5,
            "libopus": 1.2,
            "libjuice": 0.8,
            "webrtc_audio_processing": 4.0,  # APM is a separate plugin build
        }
        total_mb = (result["static_lib"]["size_bytes"] / (1024 * 1024)) + sum(
            vendored_deps_estimate_mb.values()
        )
        result["link_footprint_estimate_mb"] = round(total_mb, 2)
        result["vendored_deps_estimate_mb"] = vendored_deps_estimate_mb

    return result


def render_markdown(result: dict) -> str:
    """Render a ready-to-paste markdown table for the README."""
    lines = []
    lines.append("| Artifact | Size |")
    lines.append("|---|---|")
    if result.get("static_lib"):
        sl = result["static_lib"]
        lines.append(f"| Static lib `nimrtc_engine` | **{sl['size_human']}** |")
    else:
        lines.append("| Static lib `nimrtc_engine` | _(not built)_ |")
    for ex in result.get("examples", []):
        lines.append(f"| Example `{ex['name']}` | {ex['size_human']} |")
    if "link_footprint_estimate_mb" in result:
        mb = result["link_footprint_estimate_mb"]
        lines.append(f"| **Estimated static-link footprint** | **{mb:.1f} MB** |")
    lines.append("")
    lines.append("> Measured on: `" + result["platform"] + "`")
    lines.append(
        "> Re-run with `python tools/benchmark_size.py --markdown` after each build."
    )
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Measure NimRTC binary sizes and report embeddability numbers."
    )
    ap.add_argument(
        "build_dir",
        nargs="?",
        default="build",
        help="Build directory (default: build/)",
    )
    ap.add_argument(
        "--json",
        action="store_true",
        help="Output JSON instead of human-readable text",
    )
    ap.add_argument(
        "--markdown",
        action="store_true",
        help="Output a markdown table ready for the README",
    )
    args = ap.parse_args()

    build_dir = Path(args.build_dir).resolve()
    if not build_dir.exists():
        print(f"error: build directory '{build_dir}' does not exist", file=sys.stderr)
        print("hint: run `cmake --preset debug.msvc && cmake --build build` first",
              file=sys.stderr)
        return 1

    result = measure(build_dir)

    if args.json:
        print(json.dumps(result, indent=2))
        return 0

    if args.markdown:
        print(render_markdown(result))
        return 0

    # Default: human-readable
    print("=" * 60)
    print("NimRTC Binary Size Report")
    print("=" * 60)
    print(f"Build dir : {result['build_dir']}")
    print(f"Platform  : {result['platform']}")
    print()

    if result["static_lib"]:
        sl = result["static_lib"]
        print(f"Static lib : {sl['path']}")
        print(f"  Size    : {sl['size_human']}")
    else:
        print("Static lib : NOT FOUND (build nimrtc_engine target first)")
    print()

    if result["examples"]:
        print("Examples:")
        for ex in result["examples"]:
            print(f"  {ex['name']:15s} {ex['size_human']:>12s}  ({ex['path']})")
    else:
        print("Examples: none built (try -DNIMRTC_BUILD_EXAMPLES=ON)")
    print()

    print("-" * 60)
    print("Comparison vs libwebrtc (public reference numbers)")
    print("-" * 60)
    print(f"  NimRTC static lib         : "
          f"{result['static_lib']['size_human'] if result['static_lib'] else 'n/a'}")
    print(f"  libwebrtc static lib      : "
          f"{LIBWEBRTC_REFERENCE['static_lib_min_mb']}–"
          f"{LIBWEBRTC_REFERENCE['static_lib_max_mb']} MB (typical)")
    print(f"  libwebrtc with all codecs : "
          f"~{LIBWEBRTC_REFERENCE['monolithic_with_codecs_mb']} MB")
    if "link_footprint_estimate_mb" in result:
        print()
        print(f"  NimRTC estimated static-link footprint  : "
              f"~{result['link_footprint_estimate_mb']:.1f} MB")
        print(f"  libwebrtc estimated static-link footprint: "
              f"~{LIBWEBRTC_REFERENCE['minimal_peerconnection_estimate_mb']}+ MB")
    print()
    print("Re-run with --markdown to get a README-ready table.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
