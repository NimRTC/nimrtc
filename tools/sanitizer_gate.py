#!/usr/bin/env python3
"""Classify sanitizer reports found in a test log and fail on NimRTC-owned ones.

Why this script exists
----------------------
The CI sanitizer leg (linux-sanitizers in .github/workflows/ci.yml) runs the
test suite under AddressSanitizer / UndefinedBehaviorSanitizer /
ThreadSanitizer.  The vendored third-party stacks -- wolfSSL, usrsctp,
libjuice, msquic, libopus, mbedTLS -- are upstream code and do trip the
sanitizers on their own.  A gate that fails on every upstream finding
teaches everyone to ignore the gate; a gate that ignores everything finds
nothing.

So every report is classified by *ownership of the root frame*:

  owned    the root frame points into NimRTC's own sources (src/... or
           tests/...).  A NimRTC defect; fails the gate.
  vendor   the root frame points into src/third_party/...  Upstream code;
           reported loudly, does not fail the gate by default.
  shared   the root frame is vendored, but a *second* stack of the same
           report is rooted in NimRTC code -- NimRTC code is the other
           party to the fault (the typical shape of a data race: our
           write versus an upstream write of the same location).
           Reported loudly; fails only with --fail-on-shared.
  unknown  no frame could be attributed to either tree.  Reported, never
           fails -- there is nothing to attribute.

A report is split into stacks: a stack starts at a "Write of size ..." /
"Read of size ..." / "Previous write ..." / "Location is ..." header and
runs to the next such header.  The root frame of a stack is its innermost
frame that points into either tree.  The root frame of the *report* is the
root of the first stack -- for a memory error that is the faulting
instruction, for a data race that is the access that was detected first.
Frames deeper in the same stack (the callers of the faulting function)
are participants, not causes: a vendored use-after-free that our code
happens to call into is still an upstream bug.

The ctest exit status can be handed in with --ctest-status.  A failed test
run is only tolerated when an upstream-rooted report explains it (a
sanitizer abort in vendored code kills the test binary).  A failure that no
upstream report explains is a plain test failure and fails the gate -- the
sanitizer build can expose timing bugs the plain build does not.

Usage:
    python tools/sanitizer_gate.py build/sanitizer-ctest.log [more.log ...]
    python tools/sanitizer_gate.py --ctest-status build/ctest-exit.txt log.txt
    python tools/sanitizer_gate.py --self-test
    cat log | python tools/sanitizer_gate.py -

Exit codes:
    0  no NimRTC-owned finding and no unexplained test failure
    1  a NimRTC-owned finding, a shared finding under --fail-on-shared,
       or a test failure no upstream report explains
    2  usage or I/O error (fails closed)
"""

import os
import re
import sys

# --------------------------------------------------------------------------
# Report detection
# --------------------------------------------------------------------------

# A report starts at one of these lines.  "SUMMARY:" lines are excluded:
# they repeat a report we already counted.
REPORT_HEADS = (
    # GCC/Clang prefix the ASan/LeakSanitizer lines with "==<pid>==".
    ("asan", re.compile(r"(?:^|==\d+==)(?:ERROR|WARNING): (?:Address|Leak|Memory)Sanitizer:")),
    ("tsan", re.compile(r"(?:^|==\d+==)WARNING: ThreadSanitizer:")),
    ("ubsan", re.compile(r"runtime error:")),
)

REPORT_TAIL = re.compile(r"^\s*SUMMARY: (?:Address|Leak|Memory|Thread|Undefined)")

# A stack inside a report starts at one of these headers.
STACK_HEAD = re.compile(
    r"^\s*(?:Write of size|Read of size|Atomic write|Atomic read"
    r"|Previous write|Previous read|Location is|Previous atomic)"
)

# A source location: an optional drive letter, a path, an extension from the
# list, and optional :line:col.  Sanitizer runtime frames (libasan.so.6,
# libc.so.6, ld-linux) never match because ".so" is not in the list.
SOURCE_RE = re.compile(
    r"(?P<path>(?:[A-Za-z]:[\\/])?[^()\s:,\[\]]*?[\w\-+\.]+"
    r"\.(?:cpp|cxx|c\+\+|cc|c|hpp|hxx|hh|h|ipp|tcc|m|mm))(?![A-Za-z0-9])"
    r"(?::(?P<line>\d+))?"
    r"(?::(?P<col>\d+))?"
)

MAX_BLOCK_LINES = 400


def _classify_path(path):
    """Map a source path to 'owned', 'vendor' or 'unknown'."""
    norm = path.replace("\\", "/")
    if "/src/third_party/" in norm or norm.startswith("src/third_party/"):
        return "vendor"
    if "/src/" in norm or norm.startswith("src/"):
        return "owned"
    if "/tests/" in norm or norm.startswith("tests/"):
        return "owned"
    return "unknown"


def _split_blocks(lines):
    """Yield (kind, [lines]) for every sanitizer report in the log."""
    blocks = []
    i = 0
    total = len(lines)
    while i < total:
        line = lines[i]
        if line.lstrip().startswith("SUMMARY:"):
            i += 1
            continue
        kind = None
        for name, pattern in REPORT_HEADS:
            if pattern.search(line):
                kind = name
                break
        if kind is None:
            i += 1
            continue

        block = [line]
        j = i + 1
        while j < total and len(block) < MAX_BLOCK_LINES:
            nxt = lines[j]
            if REPORT_TAIL.match(nxt):
                block.append(nxt)
                j += 1
                break
            head = any(p.search(nxt) for _, p in REPORT_HEADS)
            if head and not nxt.lstrip().startswith("SUMMARY:"):
                break
            block.append(nxt)
            j += 1
        blocks.append((kind, block))
        i = j
    return blocks


def _split_stacks(block):
    """Split a report block into its stacks."""
    stacks = []
    current = []
    for line in block:
        if STACK_HEAD.match(line) and current:
            stacks.append(current)
            current = []
        current.append(line)
    if current:
        stacks.append(current)
    return stacks


def _locations(lines):
    """All source locations in a list of lines, in order of appearance."""
    out = []
    for line in lines:
        for match in SOURCE_RE.finditer(line):
            out.append(
                (
                    match.group("path"),
                    int(match.group("line") or 0),
                    int(match.group("col") or 0),
                )
            )
    return out


def analyze(text):
    """Classify every sanitizer report in `text`.

    Returns a list of dicts: kind, verdict ('owned'/'vendor'/'shared'/
    'unknown'), root (path, line, col) or None, participants (list of
    distinct NimRTC-owned paths), header (the first line of the report).
    """
    lines = text.splitlines()
    findings = []
    for kind, block in _split_blocks(lines):
        stacks = _split_stacks(block)

        roots = []
        participants = []
        for stack in stacks:
            locs = _locations(stack)
            stack_root = None
            for path, line, col in locs:
                owner = _classify_path(path)
                if owner == "owned" and path not in participants:
                    participants.append(path)
                if owner == "unknown":
                    continue
                if stack_root is None:
                    stack_root = (path, line, col)
            if stack_root is not None:
                roots.append(stack_root)

        root = roots[0] if roots else None
        if root is None:
            verdict = "unknown"
        elif _classify_path(root[0]) == "owned":
            verdict = "owned"
        else:
            # The first stack is vendored.  If another stack is rooted in
            # NimRTC code, NimRTC is the other party to the fault.
            verdict = "vendor"
            for other in roots[1:]:
                if _classify_path(other[0]) == "owned":
                    verdict = "shared"
                    break

        findings.append(
            {
                "kind": kind,
                "verdict": verdict,
                "root": root,
                "participants": participants,
                "header": block[0].strip(),
                "block": block,
            }
        )
    return findings


# --------------------------------------------------------------------------
# Reporting
# --------------------------------------------------------------------------


def _fmt(loc):
    if loc is None:
        return "<unattributed>"
    path, line, col = loc
    if line:
        return "%s:%d" % (path, line) + (":%d" % col if col else "")
    return path


def report(findings, fail_on_shared, ctest_status=None):
    owned = [f for f in findings if f["verdict"] == "owned"]
    shared = [f for f in findings if f["verdict"] == "shared"]
    vendor = [f for f in findings if f["verdict"] == "vendor"]
    unknown = [f for f in findings if f["verdict"] == "unknown"]

    print("== sanitizer gate ==")
    for f in findings:
        print("[%s] %s" % (f["verdict"], f["header"]))
        print("    root: %s" % _fmt(f["root"]))
        if f["participants"] and f["verdict"] != "owned":
            for p in f["participants"]:
                print("    nimrtc frame: %s" % p)

    print(
        "-- %d report(s): %d owned, %d shared, %d vendor, %d unattributed"
        % (len(findings), len(owned), len(shared), len(vendor), len(unknown))
    )

    failed = False

    if owned:
        failed = True
        print("FAIL: %d NimRTC-owned sanitizer finding(s):" % len(owned))
        for f in owned:
            print("  - %s at %s" % (f["header"], _fmt(f["root"])))
            for line in f["block"][:40]:
                print("      %s" % line.rstrip())

    if shared:
        if fail_on_shared:
            failed = True
            print("FAIL: %d report(s) where NimRTC code participates:" % len(shared))
            for f in shared:
                print("  - %s at %s" % (f["header"], _fmt(f["root"])))
        else:
            print(
                "note: %d report(s) are upstream-rooted with NimRTC code as "
                "the other party to the fault." % len(shared)
            )

    if ctest_status is not None and ctest_status != 0:
        explained = bool(vendor) or bool(shared)
        if explained:
            print(
                "note: the test run failed (exit %d) and an upstream sanitizer "
                "finding explains it." % ctest_status
            )
        else:
            failed = True
            print(
                "FAIL: the test run failed (exit %d) and no upstream sanitizer "
                "finding explains it -- this is a plain test failure." % ctest_status
            )

    if failed:
        return 1
    print("PASS: no NimRTC-owned sanitizer finding.")
    return 0


# --------------------------------------------------------------------------
# Self-test
# --------------------------------------------------------------------------

OWNED_ASAN = """
=================================================================
==12345==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x602000000010
READ of size 4 at 0x602000000010 thread T0
    #0 0x4f1a2b in nimrtc::rtp::Demuxer::Consume() src/modules/rtp/src/demuxer.cpp:88:12
    #1 0x4f22c1 in nimrtc::engine::Tick() src/engine/src/engine_media.cpp:210:5
    #2 0x7f8e in main() /usr/include/c++/12/stdlib.h:1:1
SUMMARY: AddressSanitizer: heap-buffer-overflow src/modules/rtp/src/demuxer.cpp:88:12 in nimrtc::rtp::Demuxer::Consume()
"""

VENDOR_ASAN = """
=================================================================
==12345==ERROR: AddressSanitizer: heap-use-after-free on address 0x60200000ff00
READ of size 8 at 0x60200000ff00 thread T0
    #0 0x4f1a2b in usrsctp_read() src/third_party/usrsctp/usrsctplib/netinet/sctp_callon.c:512:9
    #1 0x4f22c1 in nimrtc::sctp::OnRead() src/transport/src/raw_udp_arq_stack.cpp:301:5
SUMMARY: AddressSanitizer: heap-use-after-free src/third_party/usrsctp/usrsctplib/netinet/sctp_callon.c:512:9 in usrsctp_read()
"""

OWNED_UBSAN = """
src/core/src/engine_errors.cpp:64:9: runtime error: signed integer overflow: 2147483647 + 1 cannot be represented in type 'int'
    #0 0x4f1a2b in nimrtc::core::ToErrorCode() src/core/src/engine_errors.cpp:64:9
    #1 0x4f22c1 in main() /usr/include/c++/12/stdlib.h:1:1
"""

SHARED_TSAN = """
==================
WARNING: ThreadSanitizer: data race (pid=1234)
  Write of size 4 at 0x7b0400000000 by thread T1:
    #0 0x4f1a2b in usrsctp_inp_set() src/third_party/usrsctp/usrsctplib/netinet/sctp_input.c:188:3
    #1 0x4f22c1 in nimrtc::sctp::Pump() src/transport/src/raw_udp_arq_stack.cpp:412:5
  Previous write of size 4 at 0x7b0400000000 by main thread T0:
    #0 0x4f1a2b in nimrtc::sctp::Pump() src/transport/src/raw_udp_arq_stack.cpp:412:5
SUMMARY: ThreadSanitizer: data race src/transport/src/raw_udp_arq_stack.cpp:412:5 in nimrtc::sctp::Pump()
"""

UNKNOWN_ONLY = """
=================================================================
==12345==ERROR: AddressSanitizer: SEGV on unknown address 0x000000000000
    #0 0x7f00 in __restore_rt (/usr/lib/x86_64-linux-gnu/libc.so.6+0x21b0)
"""

MULTI = OWNED_ASAN + "\n" + VENDOR_ASAN


def _self_test():
    failures = []

    def check(name, got, want):
        if got != want:
            failures.append("%s: got %r, want %r" % (name, got, want))

    f = analyze(OWNED_ASAN)
    check("asan-owned count", len(f), 1)
    check("asan-owned verdict", f[0]["verdict"], "owned")
    check("asan-owned root", f[0]["root"][0], "src/modules/rtp/src/demuxer.cpp")
    check("asan-owned root line", f[0]["root"][1], 88)

    f = analyze(VENDOR_ASAN)
    check("asan-vendor count", len(f), 1)
    check("asan-vendor verdict", f[0]["verdict"], "vendor")
    check(
        "asan-vendor root",
        f[0]["root"][0],
        "src/third_party/usrsctp/usrsctplib/netinet/sctp_callon.c",
    )
    check("asan-vendor participants", len(f[0]["participants"]), 1)

    f = analyze(OWNED_UBSAN)
    check("ubsan count", len(f), 1)
    check("ubsan verdict", f[0]["verdict"], "owned")
    check("ubsan root", f[0]["root"][0], "src/core/src/engine_errors.cpp")
    check("ubsan root line", f[0]["root"][1], 64)

    f = analyze(SHARED_TSAN)
    check("tsan count", len(f), 1)
    check("tsan verdict", f[0]["verdict"], "shared")

    f = analyze(UNKNOWN_ONLY)
    check("unknown count", len(f), 1)
    check("unknown verdict", f[0]["verdict"], "unknown")

    f = analyze(MULTI)
    check("multi count", len(f), 2)
    check("multi verdicts", sorted(x["verdict"] for x in f), ["owned", "vendor"])

    check("clean log", analyze("no problems here\n"), [])

    # The gate decisions themselves.
    check("gate owned", report(analyze(OWNED_ASAN), False), 1)
    check("gate vendor", report(analyze(VENDOR_ASAN), False), 0)
    check("gate shared", report(analyze(SHARED_TSAN), False), 0)
    check("gate shared strict", report(analyze(SHARED_TSAN), True), 1)
    check("gate clean", report([], False), 0)

    # The ctest status handling.
    check("gate status clean", report(analyze(""), False, ctest_status=0), 0)
    check(
        "gate status unexplained failure",
        report(analyze(""), False, ctest_status=1),
        1,
    )
    check(
        "gate status explained by vendor",
        report(analyze(VENDOR_ASAN), False, ctest_status=1),
        0,
    )
    check(
        "gate status explained by shared",
        report(analyze(SHARED_TSAN), False, ctest_status=1),
        0,
    )
    check(
        "gate owned beats explained status",
        report(analyze(OWNED_ASAN), False, ctest_status=1),
        1,
    )

    if failures:
        print("sanitizer_gate self-test FAILED:")
        for line in failures:
            print("  - %s" % line)
        return 1
    print("sanitizer_gate self-test passed.")
    return 0


# --------------------------------------------------------------------------
# Entry point
# --------------------------------------------------------------------------


def _read_status(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            return int(handle.read().strip() or "0")
    except (OSError, ValueError):
        return None


def main(argv):
    args = argv[1:]
    if args and args[0] == "--self-test":
        return _self_test()

    fail_on_shared = False
    status_path = None
    paths = []
    index = 0
    while index < len(args):
        arg = args[index]
        if arg == "--fail-on-shared":
            fail_on_shared = True
        elif arg == "--ctest-status":
            index += 1
            if index >= len(args):
                sys.stderr.write("sanitizer_gate: --ctest-status needs a file\n")
                return 2
            status_path = args[index]
        elif arg == "--help" or arg == "-h":
            print(__doc__)
            return 0
        elif arg.startswith("-"):
            sys.stderr.write("sanitizer_gate: unknown option %r\n" % arg)
            return 2
        else:
            paths.append(arg)
        index += 1

    if not paths:
        sys.stderr.write("sanitizer_gate: no log file given (use '-' for stdin)\n")
        return 2

    text = []
    for path in paths:
        try:
            if path == "-":
                text.append(sys.stdin.read())
            else:
                with open(path, "r", encoding="utf-8", errors="replace") as handle:
                    text.append(handle.read())
        except OSError as exc:
            sys.stderr.write("sanitizer_gate: cannot read %s: %s\n" % (path, exc))
            return 2

    ctest_status = None
    if status_path is not None:
        if not os.path.exists(status_path):
            sys.stderr.write(
                "sanitizer_gate: no ctest status file at %s\n" % status_path
            )
            return 2
        ctest_status = _read_status(status_path)
        if ctest_status is None:
            sys.stderr.write(
                "sanitizer_gate: cannot read the ctest status from %s\n" % status_path
            )
            return 2

    return report(analyze("\n".join(text)), fail_on_shared, ctest_status)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
