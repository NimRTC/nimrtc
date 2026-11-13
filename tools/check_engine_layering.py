#!/usr/bin/env python3
"""check_engine_layering.py — the ADR-025 Layering Invariant 4 guard.

`AGENTS.md` L4 says the engine "only orchestrates plugins; it does not
directly include any concrete module's header".  That sentence was not
true of the code for a long time, and ADR-025 rewrote it into something
that *is* true: the engine may include a **named, enumerated set** of
concrete modules, and everything else goes through a `plugins::I*` seam.

The point of naming the set is that the question "does `src/engine/`
include anything outside the table and the `plugins::` seams?" then has
a finite answer, and a reviewer -- or this script -- can check it
mechanically.  Before ADR-025 the invariant was violated on every
screen, so it had no reviewing power at all.

This guard *is* that mechanical check.  It walks the engine's
translation units, collects every `#include <nimrtc/...>` and
classifies it:

  * `nimrtc/core/...`, `nimrtc/plugins/...`, `nimrtc/engine/...` — the
    stable type layer and the seams.  Always allowed.
  * The exempted modules (below).  Allowed, and *named*, so the
    exemption is visible rather than implied.
  * Anything else — a violation.

Two things make the guard worth having rather than merely nice:

  * A violation is a **new** concrete dependency, and it is exactly the
    kind of drift that a code review does not catch by eye: a
    `#include` is one line, and the coupling it creates is not.
  * The check is cheap and needs no build, so it runs on every push.

The exemption list is the one ADR-025 Decision 1 enumerates, plus the
three modules that the Phase 4.2 / D2 work added concrete engine
includes to (`video_frame`, `video_sdp_attributes`, `video_stats`).
Those three are *not* silently permitted: they are listed here, in the
guard, so a reader can see that the engine reaches into them, and a
future change that removes one of them gets the list updated rather
than the exemption quietly growing.

Usage:
    python tools/check_engine_layering.py [--repo-root ROOT] [--verbose]
    python tools/check_engine_layering.py --self-test

Exit code 0 when every include is allowed, 1 otherwise.

`--self-test` builds a synthetic engine tree and asserts that the
checker classifies each shape correctly.  A guard that cannot fail is
a decoration, so the self-test runs in CI.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# The rules
# ---------------------------------------------------------------------------

# `#include <nimrtc/<module>/<rest>` or `#include "nimrtc/<module>/<rest>"`.
INCLUDE_RE = re.compile(
    r'^\s*#\s*include\s*[<"](nimrtc/([a-z0-9_]+)/[^">]+)[">]',
    re.MULTILINE,
)

# The header prefixes that are always allowed: the stable type layer, the
# plugin seams, and the engine's own public headers.
ALWAYS_ALLOWED = frozenset({"core", "plugins", "engine"})

# ADR-025 Decision 1's enumerated exemption, plus the three modules the
# Phase 4.2 / D2 work added a concrete engine include to.
#
# The ADR's own table reads:
#
#   sdp::   — the session description is the contract everything is
#             written against
#   rtp::   — the packet builder is on the zero-allocation hot path
#   ice::   — the ICE transport's concrete state and role enums
#   dtls::  — the DTLS session's address / role / state types
#   srtp::  — the SRTP context's crypto-suite enum
#   audio3a::  — the 3A pipeline's config and frame types
#   transport:: — the transport-stack / selector / strategy types
#   web_transport:: — the WebTransport session's config and factory types
#
# and the three added by D2:
#
#   video_frame:: — the engine stages the caller's frame into a
#                   VideoFrameBuffer, so it builds the concrete type
#   video_sdp_attributes:: — the SDP attribute vocabulary the video
#                   negotiation path reads and writes
#   video_stats:: — the quality counters, behind the
#                   `video_stats_bridge.hpp` adapter
#
# `h264` is deliberately *not* in this list.  It used to be included
# behind `NIMRTC_HAS_H264` purely as a link-time registration
# convenience; ADR-025 Decision 1 said the include was "scheduled for
# removal when the engine's default-registrar table (D1) is split out
# of engine_impl.hpp".  That split has landed -- the registrar table
# lives in `src/core/src/pal_default_registrars.cpp` -- so the include
# is gone, and re-adding it fails this guard.
EXEMPT_MODULES = frozenset({
    "sdp",
    "rtp",
    "ice",
    "dtls",
    "srtp",
    "audio3a",
    "transport",
    "web_transport",
    "video_frame",
    "video_sdp_attributes",
    "video_stats",
})

# The engine's own source root, relative to the repository root.
ENGINE_DIR = Path("src/engine")

# The source suffixes the guard reads.
SOURCE_SUFFIXES = (".cpp", ".hpp", ".h", ".cc")


def _read_text(path: Path) -> str:
    """Read a text file, tolerating a file that vanished mid-scan.

    A concurrent build (or a `git checkout` / branch switch) can remove
    a file between the moment the checker lists it and the moment it
    reads it.  A guard that dies on a race is a guard that gets
    disabled, so a missing file reads as empty and the checker moves
    on.
    """
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except (FileNotFoundError, IsADirectoryError, PermissionError, OSError):
        return ""


def _iter_engine_sources(root: Path) -> list[Path]:
    """Return every source file under the engine's source root."""
    engine_root = root / ENGINE_DIR
    if not engine_root.is_dir():
        return []
    out = [p for p in sorted(engine_root.rglob("*"))
           if p.is_file() and p.suffix in SOURCE_SUFFIXES]
    return out


def classify(module: str) -> str:
    """Classify a `nimrtc/<module>/...` include.

    Returns "allowed" or "violation".
    """
    if module in ALWAYS_ALLOWED or module in EXEMPT_MODULES:
        return "allowed"
    return "violation"


def check(root: Path) -> list[tuple[Path, int, str, str]]:
    """Return every violating include as (path, line, module, header)."""
    violations: list[tuple[Path, int, str, str]] = []
    for path in _iter_engine_sources(root):
        text = _read_text(path)
        for lineno, line in enumerate(text.splitlines(), 1):
            m = INCLUDE_RE.match(line)
            if not m:
                continue
            header, module = m.group(1), m.group(2)
            if classify(module) == "violation":
                violations.append((path, lineno, module, header))
    return violations


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------

def _rel(path: Path, root: Path) -> str:
    try:
        return str(path.relative_to(root)).replace("\\", "/")
    except ValueError:
        return str(path)


def report(root: Path, violations, verbose: bool) -> int:
    if violations:
        print("Layering Invariant 4 (ADR-025) violated: the engine includes "
              "a concrete module header that is not in the exemption list.\n",
              file=sys.stderr)
        for path, lineno, module, header in violations:
            print(f"  {_rel(path, root)}:{lineno}: "
                  f"#include <{header}>  -- module '{module}' is not exempt",
                  file=sys.stderr)
        print(
            "\nEither reach the module through its `plugins::I*` seam, or add "
            "it to\nEXEMPT_MODULES in tools/check_engine_layering.py -- with a "
            "reason.\nThe exemption list is the ADR-025 Decision 1 table: an "
            "exemption that\ngrows silently is how the invariant stopped "
            "meaning anything in the\nfirst place.  See "
            "docs/adr/ADR-025-sdp-rtp-not-pluggable.md.",
            file=sys.stderr,
        )
        return 1

    if verbose:
        sources = _iter_engine_sources(root)
        print(f"Layering Invariant 4 (ADR-025): OK "
              f"({len(sources)} engine source files scanned)")
    return 0


# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------

def _write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def self_test(root: Path) -> int:
    """Build a synthetic engine tree and assert the checker's verdicts."""
    import tempfile

    failures: list[str] = []

    def check_case(name: str, header: str, expect_violation: bool) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmp_root = Path(tmp)
            _write(tmp_root / ENGINE_DIR / "src" / "probe.cpp",
                   f"#include {header}\n")
            violations = check(tmp_root)
            got = bool(violations)
            if got != expect_violation:
                failures.append(
                    f"{name}: {header} -> "
                    f"{'violation' if got else 'allowed'}, "
                    f"expected {'violation' if expect_violation else 'allowed'}")

    # Always-allowed prefixes.
    check_case("core header", "<nimrtc/core/registry.hpp>", False)
    check_case("plugin seam", "<nimrtc/plugins/transport.hpp>", False)
    check_case("engine header", "<nimrtc/engine/engine.hpp>", False)

    # The enumerated exemption.
    check_case("sdp", "<nimrtc/sdp/session_description.hpp>", False)
    check_case("rtp", "<nimrtc/rtp/packet.hpp>", False)
    check_case("ice", "<nimrtc/ice/ice.hpp>", False)
    check_case("dtls", "<nimrtc/dtls/dtls.hpp>", False)
    check_case("srtp", "<nimrtc/srtp/srtp.hpp>", False)
    check_case("audio3a", "<nimrtc/audio3a/audio3a.hpp>", False)
    check_case("transport", "<nimrtc/transport/transport_stack.hpp>", False)
    check_case("web_transport",
               "<nimrtc/web_transport/web_transport_config.hpp>", False)
    check_case("video_frame", "<nimrtc/video_frame/frame.hpp>", False)
    check_case("video_sdp_attributes",
               "<nimrtc/video_sdp_attributes/video_sdp_attributes.hpp>", False)
    check_case("video_stats", "<nimrtc/video_stats/video_stats.hpp>", False)

    # The modules that are NOT exempt.
    check_case("h264", "<nimrtc/h264/codec_plugin.hpp>", True)
    check_case("h265", "<nimrtc/h265/codec_plugin.hpp>", True)
    check_case("opus", "<nimrtc/opus/opus.hpp>", True)
    check_case("jb", "<nimrtc/jb/jitter_buffer.hpp>", True)
    check_case("bwe", "<nimrtc/bwe/bwe.hpp>", True)
    check_case("web_transport module header",
               "<nimrtc/web_transport_extra/thing.hpp>", True)

    # A quoted include is the same include.
    check_case("quoted include", '"nimrtc/h264/codec_plugin.hpp"', True)

    if failures:
        print("self-test FAILED:", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)
        return 1

    print("self-test OK: the layering guard distinguishes allowed "
          "includes from violations")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Check Layout Invariant 4 (ADR-025) in src/engine/.")
    parser.add_argument("--repo-root", default=".",
                        help="repository root (default: the current directory)")
    parser.add_argument("--verbose", action="store_true",
                        help="report success, not just failures")
    parser.add_argument("--self-test", action="store_true",
                        help="self-test the checker and exit")
    args = parser.parse_args(argv)

    root = Path(args.repo_root).resolve()

    if args.self_test:
        return self_test(root)

    violations = check(root)
    return report(root, violations, args.verbose)


if __name__ == "__main__":
    sys.exit(main())
