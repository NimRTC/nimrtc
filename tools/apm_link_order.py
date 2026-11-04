#!/usr/bin/env python3
# Copyright (c) 2026 NimRTC contributors.
# SPDX-License-Identifier: MIT
"""Extract the authoritative link order of the prebuilt WebRTC APM archives.

The WebRTC Audio Processing Module (APM) is not vendored into git — it is
cloned and built out-of-tree with Meson (see ``tools/build_webrtc_apm.{cmd,sh}``).
Meson merges the webrtc-internal archives (``libbase``, ``libapi``,
``common_audio``, …) into ``libwebrtc-audio-processing-2.a``, so a consumer of
the prebuilt only has to link that archive plus the abseil archives it
depends on — *in the order Meson put them on the link line*.

That order is not something to guess: the archives are not self-contained
(every abseil library references ``absl_base``), and a single-pass linker
resolves symbols left-to-right, so a wrong order produces undefined-symbol
errors that look unrelated to the actual cause.

This helper reads the generated ``build.ninja`` and extracts the order from
the ``LINK_ARGS`` of the executable target that links the APM archive — i.e.
the exact command line Meson verified to work.  The result is written to
``<build-dir>/apm_link_order.txt`` (one archive path per line, relative to
the Meson build directory) and is consumed by
``src/third_party/webrtc_audio_processing/CMakeLists.txt`` (build-tree link
order) and ``cmake/NimRTCInstall.cmake`` (install set + order).

Usage:
    python tools/apm_link_order.py
    python tools/apm_link_order.py --build-dir <meson-build-dir>
    python tools/apm_link_order.py --build-dir <dir> --output <file>

Exit codes:
    0  the manifest was written
    1  the link order could not be determined
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")


REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BUILD_DIR = REPO_ROOT / "src" / "third_party" / "webrtc_audio_processing" / "build_meson"
DEFAULT_OUTPUT_NAME = "apm_link_order.txt"
DEFAULT_APM_ARCHIVE = "libwebrtc-audio-processing-2.a"

# `build <outputs>: cpp_LINKER <inputs> | <order-only deps>` followed by
# indented `KEY = value` lines.
_BUILD_RE = re.compile(r"^build\s+(?P<outputs>.+?):\s+(?P<rule>\S+)\s*(?P<rest>.*)$")
_VAR_RE = re.compile(r"^\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)$")
# Quoted or bare token.
_TOKEN_RE = re.compile(r'"([^"]*)"|(\S+)')


def _tokenize(value: str) -> list[str]:
    """Split a ninja variable value into tokens, honouring double quotes."""
    tokens: list[str] = []
    for match in _TOKEN_RE.finditer(value):
        quoted, bare = match.group(1), match.group(2)
        tokens.append(quoted if quoted is not None else bare)
    return tokens


def _relative_to_build_dir(path: Path, build_dir: Path) -> str | None:
    """Return `path` relative to `build_dir` in POSIX form, or None if outside."""
    try:
        return path.resolve().relative_to(build_dir.resolve()).as_posix()
    except ValueError:
        return None


def _collect_link_targets(build_dir: Path) -> list[tuple[str, list[str]]]:
    """Return (target, link_args_tokens) for every cpp_LINKER rule in build.ninja."""
    ninja = build_dir / "build.ninja"
    if not ninja.is_file():
        return []

    targets: list[tuple[str, list[str]]] = []
    current_outputs: str | None = None
    current_rule: str | None = None
    current_vars: dict[str, str] = {}

    def flush() -> None:
        if current_outputs and current_rule and current_rule == "cpp_LINKER":
            raw_args = current_vars.get("LINK_ARGS")
            if raw_args:
                targets.append((current_outputs, _tokenize(raw_args)))

    for line in ninja.read_text(encoding="utf-8", errors="replace").splitlines():
        build_match = _BUILD_RE.match(line)
        if build_match:
            flush()
            current_outputs = build_match.group("outputs").strip()
            current_rule = build_match.group("rule").strip()
            current_vars = {}
            continue
        var_match = _VAR_RE.match(line)
        if var_match and current_outputs is not None:
            current_vars[var_match.group(1)] = var_match.group(2).strip()
    flush()
    return targets


def extract_link_order(build_dir: Path, apm_archive: str) -> list[str]:
    """Return the archive paths (relative to `build_dir`) in Meson link order."""
    candidates: list[list[str]] = []
    for _target, tokens in _collect_link_targets(build_dir):
        if not any(apm_archive in token for token in tokens):
            continue
        ordered: list[str] = []
        for token in tokens:
            if not token.lower().endswith((".a", ".lib")):
                continue
            # A bare name (e.g. `winmm.lib`) is a system library, not a file
            # produced by the Meson build — only keep tokens that resolve to a
            # file inside the Meson build tree.
            candidate = Path(token)
            if not candidate.is_absolute():
                candidate = build_dir / candidate
            if not candidate.is_file():
                continue
            relative = _relative_to_build_dir(candidate, build_dir)
            if relative is None:
                continue
            if relative not in ordered:
                ordered.append(relative)
        if ordered:
            candidates.append(ordered)

    if not candidates:
        return []

    # Prefer the target that links the most archives: it is the one that
    # exercises the full dependency set.
    best = max(candidates, key=len)
    # The APM archive itself must lead — it is the archive that references
    # every abseil symbol.
    if best[0] != apm_archive:
        for index, entry in enumerate(best):
            if Path(entry).name == apm_archive:
                best.insert(0, best.pop(index))
                break
    return best


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=DEFAULT_BUILD_DIR,
        help="Meson build directory (default: %(default)s)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="Manifest file to write (default: <build-dir>/" + DEFAULT_OUTPUT_NAME + ")",
    )
    parser.add_argument(
        "--apm-archive",
        default=DEFAULT_APM_ARCHIVE,
        help="Basename of the APM archive that anchors the link (default: %(default)s)",
    )
    args = parser.parse_args(argv)

    build_dir = args.build_dir.resolve()
    if not build_dir.is_dir():
        print(f"[ERROR] Meson build directory not found: {build_dir}", file=sys.stderr)
        return 1

    order = extract_link_order(build_dir, args.apm_archive)
    if not order:
        print(
            f"[ERROR] Could not extract the APM link order from {build_dir / 'build.ninja'}.\n"
            "        The Meson build must produce an executable that links\n"
            f"        {args.apm_archive} (the upstream `run-offline` example does).",
            file=sys.stderr,
        )
        return 1

    output = args.output if args.output is not None else build_dir / DEFAULT_OUTPUT_NAME
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(order) + "\n", encoding="utf-8")

    print(f"[OK] APM link order ({len(order)} archives) -> {output}")
    for entry in order:
        print(f"       {entry}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
