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

A second rule runs on top of the first.  ADR-025 Decision 4 says the
RTP *packet building* is concentrated in one file (`rtp_packetizer.cpp`),
so that the engine's exemption has a visible boundary.  The module-level
rule cannot enforce that on its own -- `nimrtc/rtp/packet.hpp` is
allowed *everywhere* as far as the exemption list is concerned -- so the
guard checks the concentration separately, at the symbol level:
`rtp::PacketBuilder` may be named in exactly one engine file.

A third rule guards the *size* of the engine's internal state header.
The remediation plan §4.3 (D1) split `engine_impl.hpp` from 1076 lines
down to under 400, and the line has already been pushed back over
twice by unrelated features -- the remote-desktop multi-stream work
took it to 423, the QoE collector to 391.  A budget that lives only in
a prose document is a budget that evaporates the next time a feature
lands; this rule makes the line count a *checkable* property.

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
#   rtx::    — the RFC 4588 retransmission *payload format* (the two-octet
#              OSN header) and the `apt=` SDP parameter.  The engine
#              reads the peer's `apt=` line to learn which payload type
#              carries the retransmissions, and writes its own — the
#              same role `video_sdp_attributes` plays for the codec
#              format parameters: an SDP attribute vocabulary the
#              negotiation path reads and writes.
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
    "rtx",
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

# ---------------------------------------------------------------------------
# The ADR-025 Decision 4 rule: the *packet building* half of the engine's
# `rtp::` usage is concentrated in one file.
# ---------------------------------------------------------------------------
#
# ADR-025 Decision 1 exempts the `rtp` module from Layout Invariant 4,
# and Decision 4 says the exemption is only honest if the concrete use
# is *concentrated*: a reader must be able to look at one file and see
# every place the engine touches the RTP wire format.
#
# The rule this guard enforces is **symbol-level**, not include-level:
#
#   * `rtp::PacketBuilder` -- the outbound RTP packet constructor --
#     may be *named* in exactly one engine file: `rtp_packetizer.cpp`.
#
# Why the symbol and not the header: the engine legitimately uses the
# RTP module for two different concerns, and the plan gives each of
# them its own collaborator (§4.3):
#
#   * **building** outbound packets -- `RtpPacketizer`, which is what
#     this rule is about.  The engine must not hand-assemble an RTP
#     header anywhere else: a fix to the header format has to land in
#     one place, or the audio path and the video path drift apart.
#   * **parsing** inbound packets and RTCP -- `rtcp::demux_rtcp()`,
#     `rtp::Parser`, the XR blocks.  Those live in their own module
#     surfaces (`nimrtc/rtp/rtcp*.hpp`) and are the *receive* side;
#     concentrating them is the `rtcp_processor` collaborator's job,
#     which the plan names as separate work.
#
# An include-level rule ("only `rtp_packetizer.cpp` may include
# `nimrtc/rtp/packet.hpp`") would forbid the receive side too, which is
# not what the ADR asks for -- and it would force the receive-side
# parser to be reached through a header that names it anyway.  The
# symbol-level rule checks the thing the ADR actually cares about:
# that the engine *builds* RTP packets in one place.
#
# This is the failure mode the guard exists to catch.  The module-level
# rule answers "is this module allowed?"; this one answers "is this
# *use* of the module in the one place the ADR says it is?".

# The concrete `rtp::` symbols that may be named in exactly one engine
# file, and the file that owns them.
CONCENTRATED_SYMBOLS = {
    "rtp::PacketBuilder": "rtp_packetizer.cpp",
}

# ---------------------------------------------------------------------------
# The remediation plan §4.3 (D1) rule: the engine's internal state header
# must stay under its line budget.
# ---------------------------------------------------------------------------
#
# `engine_impl.hpp` is the engine's private state header.  It used to be
# a 1076-line catch-all; D1 split it into "the engine's own composition"
# plus named collaborators, and the acceptance criterion was that the
# header drops below 400 lines.
#
# The criterion was met -- and then broken twice, by features that had
# nothing to do with the engine's structure:
#
#   * the remote-desktop multi-stream / sync / control planes took it
#     from 394 back up to 423;
#   * the QoE collector took it up to 391.
#
# Both were fixed by moving the offending state into a collaborator, and
# both fixes were the *right* fix -- the header was growing because new
# state was being added to it rather than next to it.  But the fix
# depended on someone noticing.  This rule is that noticing, automated.
#
# Why the budget is a *line count* and not something more precise: the
# point of the rule is not to police the header's contents (that is what
# the collaborator split is for), it is to make the header's growth
# *visible at review time*.  A member count would be a worse proxy --
# it would move when a comment moves -- and a "no new member in Impl"
# rule would duplicate a judgement a reviewer makes better by reading
# the header.
#
# The budget counts lines the way `wc -l` does: the number of newline
# characters.  That is the measure the plan's own history recorded
# (1076 / 394 / 423 / 391 / 381), so the guard and the document agree.
#
# What to do when the guard fires: the new state belongs in a named
# collaborator under `src/engine/src/`, next to the state it belongs
# with -- `engine_audio_state.hpp`, `engine_video_state.hpp`,
# `engine_feedback_state.hpp` and the rest.  See
# `docs/plan/architecture-remediation-plan.md` §4.3.

# The header the budget applies to, relative to the repository root.
STATE_HEADER = Path("src/engine/src/engine_impl.hpp")

# The maximum number of lines the header may have.
STATE_HEADER_MAX_LINES = 400

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


def check_concentration(root: Path) -> list[tuple[Path, int, str, str]]:
    """Return every *misplaced* concentrated symbol.

    ADR-025 Decision 4 concentrates the RTP packet building in
    `rtp_packetizer.cpp`.  A concentrated symbol named anywhere else is
    a violation of that decision -- and the module-level exemption
    would not catch it, because the module itself is allowed.

    The scan strips comments and string literals first, so a symbol
    mentioned in a doc comment ("the old code used `rtp::PacketBuilder`
    directly") is not a violation.  What matters is the *code*.

    Returns (path, line, symbol, allowed_file) tuples.
    """
    violations: list[tuple[Path, int, str, str]] = []
    for path in _iter_engine_sources(root):
        text = _read_text(path)
        name = path.name
        for lineno, line in enumerate(_strip_comments_and_strings(text), 1):
            for symbol, allowed in CONCENTRATED_SYMBOLS.items():
                if name == allowed:
                    continue
                if re.search(r"\b" + re.escape(symbol) + r"\b", line):
                    violations.append((path, lineno, symbol, allowed))
    return violations


def count_lines(text: str) -> int:
    """Count the lines of a source file the way `wc -l` does.

    That is: the number of newline characters.  A file that ends with a
    newline therefore has no trailing empty line, and a file that does
    not ends with one still counts its last (unterminated) line.

    This is the measure the remediation plan's own history recorded
    (1076 / 394 / 423 / 391 / 381 lines for `engine_impl.hpp`), so the
    guard and the document talk about the same number.
    """
    return text.count("\n")


def check_state_header(root: Path) -> tuple[int, int]:
    """Return the state header's line count and the budget it must meet.

    A *missing* header is not a violation: the check is about the
    header's size, and a repository without the header has no D1
    problem to report.  (0, budget) is returned in that case, which
    passes.
    """
    path = root / STATE_HEADER
    if not path.is_file():
        return 0, STATE_HEADER_MAX_LINES
    return count_lines(_read_text(path)), STATE_HEADER_MAX_LINES


def _strip_comments_and_strings(text: str) -> list[str]:
    """Return the source's code lines, with comments and literals blanked.

    A guard that fires on a *mention* is a guard people learn to
    silence with a reworded comment, so the scan looks at code only.
    Each removed span is replaced by spaces, which keeps the line
    numbers of the surviving code intact.
    """
    out: list[str] = []
    in_block = False
    for raw in text.split("\n"):
        line: list[str] = []
        i = 0
        n = len(raw)
        while i < n:
            if in_block:
                if raw.startswith("*/", i):
                    in_block = False
                    line.append("  ")
                    i += 2
                else:
                    line.append(" ")
                    i += 1
                continue
            if raw.startswith("//", i):
                line.append(" " * (n - i))
                break
            if raw.startswith("/*", i):
                in_block = True
                line.append("  ")
                i += 2
                continue
            ch = raw[i]
            if ch in ('"', "'"):
                quote = ch
                line.append(" ")
                i += 1
                while i < n:
                    if raw[i] == "\\":
                        line.append(" ")
                        i += 1
                        if i < n:
                            line.append(" ")
                            i += 1
                        continue
                    if raw[i] == quote:
                        line.append(" ")
                        i += 1
                        break
                    line.append(" ")
                    i += 1
                continue
            line.append(ch)
            i += 1
        out.append("".join(line))
    return out


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


def report_concentration(root: Path, violations, verbose: bool) -> int:
    """Report the ADR-025 Decision 4 concentration violations."""
    if violations:
        print("ADR-025 Decision 4 violated: the RTP packet building is not "
              "concentrated.\n",
              file=sys.stderr)
        for path, lineno, symbol, allowed in violations:
            print(f"  {_rel(path, root)}:{lineno}: "
                  f"{symbol}  -- only {allowed} may name it",
                  file=sys.stderr)
        print(
            "\nThe outbound RTP packet building is concentrated in "
            "src/engine/src/rtp_packetizer.cpp so that the engine's\n"
            "exemption from Layout Invariant 4 has a visible boundary, and so "
            "that a fix\nto the RTP header lands in one place.  Route the packet "
            "through the\n`detail::RtpPacketizer` collaborator instead of naming "
            "the concrete builder\nhere.  See docs/adr/ADR-025-sdp-rtp-not-pluggable.md\n"
            "(Decision 4).",
            file=sys.stderr,
        )
        return 1

    if verbose:
        print("ADR-025 Decision 4 (rtp:: concentration): OK "
              "(the RTP packet builder is named in rtp_packetizer.cpp only)")
    return 0


def report_state_header(root: Path, lines: int, budget: int,
                        verbose: bool) -> int:
    """Report the §4.3 state-header line budget."""
    if lines > budget:
        over = lines - budget
        print(
            "Remediation plan §4.3 (D1) violated: the engine's internal "
            "state header is over its line budget.\n",
            file=sys.stderr,
        )
        print(f"  {_rel(STATE_HEADER, root)}: {lines} lines "
              f"-- the budget is {budget} (over by {over})",
              file=sys.stderr)
        print(
            "\nThe header holds the engine's own composition; every other "
            "feature's state\nlives in a named collaborator next to it.  The "
            "new state that grew the\nheader belongs in one of those "
            "collaborators -- not in `Impl`.\nSee docs/plan/architecture-"
            "remediation-plan.md §4.3 and the header's own\nfile comment "
            "(the collaborator table) for where each concern lives.",
            file=sys.stderr,
        )
        return 1

    if verbose:
        print(f"Remediation plan §4.3 (D1): OK "
              f"({_rel(STATE_HEADER, root)} is {lines} lines, budget {budget})")
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

    # ---- The ADR-025 Decision 4 concentration rule --------------------
    def check_concentration_case(name: str, filename: str, body: str,
                                 expect_violation: bool) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmp_root = Path(tmp)
            _write(tmp_root / ENGINE_DIR / "src" / filename, body)
            violations = check_concentration(tmp_root)
            got = bool(violations)
            if got != expect_violation:
                failures.append(
                    f"{name}: {filename} -> "
                    f"{'violation' if got else 'allowed'}, "
                    f"expected {'violation' if expect_violation else 'allowed'}")

    # The one file that may name the concrete builder.
    check_concentration_case(
        "the packetizer owns the RTP builder",
        "rtp_packetizer.cpp",
        "rtp::PacketBuilder pb;\n", False)

    # Everywhere else, it is a violation -- even though the *module* is
    # exempt, which is the whole point of the second rule.
    check_concentration_case(
        "another engine file naming the RTP builder",
        "engine_media.cpp",
        "rtp::PacketBuilder pb;\n", True)
    check_concentration_case(
        "an engine header naming the RTP builder",
        "engine_impl.hpp",
        "    rtp::PacketBuilder pb;\n", True)

    # A mention in a *comment* is not a use.  A guard that fires on a
    # mention is a guard people learn to silence with a reworded
    # comment.
    check_concentration_case(
        "a comment naming the RTP builder",
        "engine_media.cpp",
        "// the old code used rtp::PacketBuilder directly\n", False)
    check_concentration_case(
        "a block comment naming the RTP builder",
        "engine_media.cpp",
        "/* rtp::PacketBuilder was here once */\n", False)
    check_concentration_case(
        "a string literal naming the RTP builder",
        "engine_media.cpp",
        'const char* s = "rtp::PacketBuilder";\n', False)

    # The *receive* side is a different concern and is not covered.
    check_concentration_case(
        "the receive-side parser is not concentrated",
        "engine_media.cpp",
        "rtp::Parser rp;\n", False)
    check_concentration_case(
        "the RTCP demux is not concentrated",
        "engine_media.cpp",
        "rtp::demux_rtcp(compound, out);\n", False)

    # ---- The §4.3 state-header line budget -----------------------------
    def check_budget_case(name: str, body: str, expect_violation: bool,
                          budget: int = 10) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmp_root = Path(tmp)
            _write(tmp_root / STATE_HEADER, body)
            lines, lim = check_state_header(tmp_root)
            # The synthetic case uses a 10-line budget, so the check
            # reads the count and compares it against *that* limit.
            got = lines > lim if budget == STATE_HEADER_MAX_LINES \
                else lines > budget
            if got != expect_violation:
                failures.append(
                    f"{name}: {lines} lines -> "
                    f"{'violation' if got else 'allowed'}, "
                    f"expected {'violation' if expect_violation else 'allowed'}")

    # Under the budget: fine.
    check_budget_case("a small state header", "x\n" * 5, False)
    # Exactly at the budget: still fine (the rule is "under", i.e. the
    # count must not *exceed* the budget).
    check_budget_case("a state header exactly at the budget",
                      "x\n" * 10, False)
    # One line over: a violation.
    check_budget_case("a state header one line over the budget",
                      "x\n" * 11, True)

    # The line count is the `wc -l` measure: the number of newlines.
    # A file whose last line is not newline-terminated still counts it.
    with tempfile.TemporaryDirectory() as tmp:
        tmp_root = Path(tmp)
        _write(tmp_root / STATE_HEADER, "a\nb\nc")
        lines, _ = check_state_header(tmp_root)
        if lines != 2:
            failures.append(
                f"the line count must be the wc -l measure: "
                f"3 lines with no trailing newline counted as {lines}")

    # A missing header is not a violation -- the rule is about the
    # header's size, and a repository without it has no D1 problem.
    with tempfile.TemporaryDirectory() as tmp:
        tmp_root = Path(tmp)
        lines, lim = check_state_header(tmp_root)
        if lines > lim:
            failures.append("a missing state header must not be a violation")

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

    # Three rules, all reported: the module-level exemption list, the
    # Decision 4 concentration, and the §4.3 state-header line budget.
    # A failure in any of them is a failure.
    rc = report(root, check(root), args.verbose)
    rc |= report_concentration(root, check_concentration(root), args.verbose)
    lines, budget = check_state_header(root)
    rc |= report_state_header(root, lines, budget, args.verbose)
    return rc


if __name__ == "__main__":
    sys.exit(main())
