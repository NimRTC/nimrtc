"""Check the v1.4 plan's claims against the tree.

The plan's §10 lists the claims as a verification table.  Each row is a
`rg` a person is meant to run by hand, and hand-run checks drift: the
tree moves, the plan does not, and the next reader follows a table that
describes a tree that no longer exists.

So the table is *executed* here.  Every row is a probe -- a search that
must come back empty (the feature is absent) or non-empty (it is
present) -- and the report is the verdict per row.

The point is not to assert the plan is right.  It is to make the drift
*visible*: a row whose expectation no longer holds is the row that
describes a plan that is out of date, and it is reported as such.

Usage:
    python tools/check_v14_plan_status.py            # the report
    python tools/check_v14_plan_status.py --json
    python tools/check_v14_plan_status.py --strict   # non-zero on STALE
    python tools/check_v14_plan_status.py --self-test

The flags are the same as `check_v15_plan_status.py`'s, and so is the
meaning of every one of them: two checkers for two plans, one interface.
CI runs both the same way -- `--self-test` and the plain report -- and
neither is run with `--strict`, for the reason given at the CI step:
a plan that has moved on is a documentation task, and a build that goes
red for it teaches people to ignore the build.  `--strict` exists for
the reader who wants the drift to be an error (a release preflight,
say); it is a choice they make, not one the build makes for them.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


# --------------------------------------------------------------------------
# The probes
# --------------------------------------------------------------------------
#
# Each row is: (id, what the plan claims, expect, search-where, pattern)
#
#   expect "absent"  -- the pattern must NOT be found
#   expect "present" -- the pattern must be found
#
# "present" is the interesting one: it is how a *finished* item is
# recognised.  A plan that says "this does not exist" and a tree that
# has it is a plan that is out of date -- and the row says so rather
# than quietly passing.
#
# The consequence is that **a row's expectation flips when its item
# lands**.  N1, N5 and D2 all closed on 2026-11-24, and their rows
# were flipped from "absent" to "present" at that point: the claim
# they were checking ("this does not exist") had stopped being true,
# and the row's job became to *confirm* the thing that replaced it.
# The claim text in the table below is kept in the plan's own past
# tense where the plan still reads that way, so a reader can match
# the row against §10's wording; what the row asserts is the tree's
# current state.

PROBES: list[tuple[str, str, str, str, str]] = [
    # -- N1: real TURN/TLS ---------------------------------------------
    ("N1", "the ICE module carries a TURN/TLS client", "present",
     "src/modules/ice", "turn_tls|TurnTls"),
    ("N1", "the TURN/TLS client has a test", "present",
     "src/modules/ice/tests", "turn_tls|TurnTls"),

    # -- N2: ICE-over-TCP candidates -----------------------------------
    #
    # N2 closed with slice `v2-ice` (ADR-028), so the claims flipped:
    # the TCP candidate shape now EXISTS, and the probe asks for it
    # rather than for its absence.  The wording below is the
    # *historical* claim -- the plan's §1 still reads this way -- and
    # what the row asserts now is that the tree carries the thing the
    # claim said was missing.
    #
    # The two rows probe the two halves of the slice: the *shape* (the
    # `tcptype` token and the RFC 6544 serialisation) lives in the ice
    # module's `tcp_candidate.hpp`, and the ICE-over-TCP transport
    # itself lives in the ice module's `tcp_ice.cpp` / `tcp_socket.cpp`.
    # The SDP module is deliberately NOT probed for `tcptype`: the
    # candidate's SDP line is built by the ice module (the candidate
    # owns its own serialisation), so the SDP module still has no
    # `tcptype` of its own -- which is exactly what the original
    # "absent" row asserted.
    ("N2", "there are no TCP candidates", "present",
     "src/modules/ice", "tcptype"),
    ("N2", "there are no TCP candidates (ice)", "present",
     "src/modules/ice", "tcp_ice|tcp_socket"),

    # -- N3: the reconnection policy -----------------------------------
    # The *declaration* and the *definition* of `restart_ice()` are the
    # API, not a policy: the claim is that nothing calls it *by
    # itself*.  So the probe looks for the policy itself -- the named
    # collaborator the plan says N3 introduces.  A file that does not
    # exist is the honest way to say "no policy is here".
    ("N3", "there is no reconnection policy in the engine", "present",
     "src/engine/src", "engine_ice_recovery"),

    # -- N4: connectivity diagnostics ----------------------------------
    #
    # N4 closed on 2026-11-27, so the claims flipped the way N5's and
    # D2's did: the things N4 was about to add now EXIST, and the probe
    # asks for them rather than for their absence.  The wording below is
    # the *historical* claim -- the plan's §1 still reads this way -- and
    # what the row asserts now is that the tree carries the thing the
    # claim said was missing.
    ("N4", "the stats carry a failure-reason vocabulary", "present",
     "src/engine", "ice_failure_reason"),
    ("N4", "the stats carry a gathering-state field", "present",
     "src/engine", "ice_gathering_state"),
    ("N4", "the stats name the TURN transport", "present",
     "src/engine", "turn_transport_used"),

    # -- N5: the reference TURN deployment -----------------------------
    #
    # N5 closed on 2026-11-24, so the claim flipped: the deployment
    # now EXISTS, and the probe asks for it rather than for its
    # absence.  The wording below is the *historical* claim -- the
    # table in the plan still reads this way -- and what the row
    # asserts now is that the tree carries the thing the claim said
    # was missing.
    ("N5", "the reference TURN deployment exists", "present",
     "examples", "turn-server"),

    # -- D6: the connectivity profile ----------------------------------
    #
    # This row used to live under **N6**, and that was a filing error:
    # §1.1's "no connectivity profile" is D6's claim (D6 *is* the
    # connectivity profile), while N6 is the SDK *sample* -- a different
    # thing, with a different bar (a ≤ 200-line application that does
    # the whole call).  So the row fired on D6's landing and reported
    # N6 as stale, which was both wrong (N6 is not done) and a
    # permanent false alarm (D6 landed, so the row could never go back
    # to "absent").
    #
    # D6 closed on 2026-11-27: `EngineConfig::connectivity_profile`
    # exists, and the profile table lives beside it.
    ("D6", "the engine has a connectivity profile", "present",
     "src/engine", "connectivity_profile"),
    ("D6", "the connectivity profile table exists", "present",
     "src/engine/src", "resolve_connectivity_profile"),

    # -- N6: the SDK sample ---------------------------------------------
    #
    # N6 is NOT closed: the ≤ 200-line sample that does the whole call
    # through a TLS-relayed NAT by default config does not exist yet.
    # The row asks for the thing N6 is about to add -- the sample's
    # own directory under examples/ -- so that it flips when N6 lands.
    ("N6", "the SDK sample exists", "present",
     "examples", "sdk-sample"),

    # -- N7: credentials as a seam -------------------------------------
    ("N7", "there is no credential provider", "present",
     "src/engine", "turn_credentials"),

    # -- N8: the path change is reported -------------------------------
    #
    # N8 closed with slice `v1-path-change-report` (ADR-033), so the
    # claim flipped: the path-change report now EXISTS, and the probe
    # asks for it rather than for its absence.
    #
    # The report lands in two places: the engine's private
    # `PathChangeTracker` collaborator (which classifies the change and
    # keeps the previous-pair ledger) and the public stats fields
    # (`last_path_change_reason` / `last_path_change_new_pair`).
    ("N8", "the local recovery is not reported", "present",
     "src/engine", "path_change|PathChange"),

    # -- N9: library packaging ----------------------------------------
    #
    # N9 closed with slice `v1-macos-install-proof` (ADR-034): the
    # staged-install + `find_package(NimRTC)` consumer step now runs on
    # all three legs, macOS included.  The probe asks for the macOS
    # install-proof slice's own artifact -- the out-of-tree consumer
    # directory the leg builds against.
    ("N9", "find_package is proven on two of the three legs", "present",
     "specs/slices", "v1-macos-install-proof"),

    # -- D1: the engine runs the device layer --------------------------
    #
    # D1 closed on 2026-11-27: the engine owns the audio device
    # layer, so the claim flipped the same way N5's and D2's did.
    # "present" is how a *finished* item is recognised -- a plan
    # that says "this does not exist" and a tree that has it is a
    # plan that is out of date, and the row says so rather than
    # quietly passing.
    ("D1", "the engine runs the audio device layer", "present",
     "src/engine", "IAudioSource|IAudioSink"),
    ("D1", "the engine has an audio-source config key", "present",
     "src/engine", "audio_source_name"),
    ("D1", "the engine has an audio-sink config key", "present",
     "src/engine", "audio_sink_name"),

    # -- D2: the engine runs the renderer ------------------------------
    #
    # D2 closed on 2026-11-24: the engine owns the sink, so the claim
    # flipped the same way N5's did.  "present" is how a *finished*
    # item is recognised -- a plan that says "this does not exist" and
    # a tree that has it is a plan that is out of date, and the row
    # says so rather than quietly passing.
    ("D2", "the engine owns the video sink", "present",
     "src/engine", "video_sink_name"),

    # -- D3: the default device policy ---------------------------------
    #
    # D3 closed with slice `v1-device-policy` (ADR-040), so the claim
    # flipped the way N5's and D2's did: the per-platform default table
    # now lives in the **engine**, and the probe asks for the thing
    # that replaced it rather than for its absence.
    #
    # The wording below is the *historical* claim -- the plan's §1 still
    # reads this way -- and what the row asserts now is that the tree
    # carries the thing the claim said was missing.  Two rows, because
    # the slice did two things: the table moved, and the engine learned
    # to resolve a backend out of it.
    #
    # The table's *home* is the engine's private collaborator
    # (`src/engine/src/engine_device_policy.{hpp,cpp}`); the public
    # surface (`NimRTCEngine::default_device_policy()`) is what a host
    # reads, so the probe looks for both names.
    ("D3", "the default capture policy lives in the engine", "present",
     "src/engine", "default_device_policy"),
    ("D3", "the engine resolves a capture backend from the policy",
     "present", "src/engine", "resolve_capture_backend"),

    # -- D4: hot-plug ---------------------------------------------------
    #
    # D4 closed with the same slice, so its claim flipped on the same
    # principle: the engine now watches the capture device list and
    # re-opens a device that went away, and the optional capability
    # seam (`ICaptureDeviceEventSource`) lets a device report the change
    # itself instead of waiting for the engine's poll.
    #
    # The three rows are the three things the slice added: the poll, the
    # re-open, and the seam.  They are probed in the engine and in the
    # plugin seam separately, because the seam is a *public* header and
    # the engine's use of it is private -- one of the two can be lost
    # without the other noticing.
    ("D4", "the engine detects capture device changes", "present",
     "src/engine", "poll_capture_device_changes"),
    ("D4", "the engine re-opens a capture device", "present",
     "src/engine/src", "engine_capture_reconnect"),
    ("D4", "the device-change seam exists", "present",
     "src/plugins", "ICaptureDeviceEventSource"),

    # -- D5: DTX coupling ----------------------------------------------
    ("D5", "DTX is not coupled to concealment", "present",
     "src/modules/audio_rx", "dtx"),

    # -- D7: the app-convenience surface -------------------------------
    ("D7", "there is no call-state callback", "present",
     "src/engine", "set_on_call_state|on_call_state"),
]


def search(where: str, pattern: str) -> list[str]:
    """Every match of `pattern` under `where`, as repo-relative paths."""
    proc = subprocess.run(
        ["git", "grep", "-l", "-E", pattern, "--", where],
        cwd=ROOT, capture_output=True, text=True)
    if proc.returncode not in (0, 1):
        return []
    return [ln for ln in proc.stdout.splitlines() if ln.strip()]


def closed_items(text: str) -> set[str]:
    """The items the plan's tables declare CLOSED.

    A row in the plan's §0a looks like::

        | **D3 + D4** default device policy, camera + hot-plug | ... | **CLOSED.** ... |

    -- the item id may be **one** id or a **group** of them, because a
    slice can close two items at once.  A guard that only understood
    the single-id form would not see D3 and D4 as closed, and would
    therefore not protect their rows: the very check that is supposed
    to keep a landed item's row flipped to "present" would go blind on
    exactly the items that share a slice with another.

    So the id cell is split on "+" and every part is returned.  The
    cell is the *first* column, and the CLOSED marker is in the third,
    which is why the pattern has to skip one column between them.
    """
    found: set[str] = set()
    for match in re.finditer(
        r"\|\s*\*\*([A-Z]\d+(?:\s*\+\s*[A-Z]\d+)*)\*\*[^|]*\|[^|]*\|"
        r"\s*\*\*CLOSED",
        text,
    ):
        for part in re.split(r"\s*\+\s*", match.group(1)):
            found.add(part)
    return found


def report_lines(
    probes: list[tuple[str, str, str, str, str]] | None = None,
) -> tuple[list[str], list[str]]:
    """The verdict per row, and the list of rows that no longer hold."""
    lines: list[str] = []
    stale: list[str] = []
    for item, claim, expect, where, pattern in (
        PROBES if probes is None else probes
    ):
        hits = search(where, pattern)
        found = bool(hits)
        ok = (expect == "present") == found
        mark = "ok  " if ok else "STALE"
        note = ""
        if not ok:
            note = "   <- %s" % ", ".join(hits[:3])
            stale.append("%s: %s" % (item, claim))
        lines.append("  %s  %-4s %s%s" % (mark, item, claim, note))
    return lines, stale


def self_test() -> int:
    """The report must be able to say 'stale', and must not say it wrongly.

    The checker's whole value is that it *can* report a stale row.  A
    checker that always passes is worse than no checker, because the
    next reader trusts it.  So the self-test pins the **verdict
    machinery** -- the four cases of (expectation x found) -- rather
    than any particular row:

      - "present" + found     -> ok
      - "present" + not found -> stale
      - "absent"  + found     -> stale
      - "absent"  + not found -> ok

    It also pins the one property that makes the table *true*: an item
    the plan declares CLOSED must have its row flipped to "present".
    A row left at "absent" after its item landed reports "STALE"
    forever -- a permanent false alarm, which is how a checker teaches
    people to ignore it.

    What it deliberately does **not** pin is the currency of the open
    items: the table drifts as items land, and that is the whole point
    of the checker.  A self-test that said "D2 must be stale" would
    fail the moment D2 landed, and a self-test that fails when the
    tree improves is a self-test that trains people to ignore it.
    """
    failures = []

    def check(condition: bool, message: str) -> None:
        if not condition:
            failures.append(message)

    # A search that cannot match must find nothing.
    check(search("src/engine", "zzz_no_such_symbol_zzz") == [],
          "a search for a symbol that does not exist must come back empty")
    # A search for something that does must find it.
    check(search("src/engine", "video_sink_name") != [],
          "a search for a symbol that exists must come back non-empty")
    # The directory argument is honoured.
    check(search("src/modules/audio_rx", "zzz_no_such_symbol_zzz") == [],
          "a search in a directory must come back empty when nothing "
          "matches")

    # The report must run, and it must be able to report staleness.
    lines, stale = report_lines()
    check(len(lines) == len(PROBES),
          "every probe must be reported")
    check(isinstance(stale, list),
          "the report must return the stale list")

    # The verdict machinery: all four cases, each with the verdict
    # written out by hand.  "video_sink_name" is in the tree;
    # "zzz_no_such_symbol_zzz" is not.
    present_symbol = "video_sink_name"
    missing_symbol = "zzz_no_such_symbol_zzz"
    cases = [
        # (expect, pattern, must_be_stale)
        ("present", present_symbol, False),
        ("present", missing_symbol, True),
        ("absent", present_symbol, True),
        ("absent", missing_symbol, False),
    ]
    for expect, pattern, must_be_stale in cases:
        probes = [("ZZ", "synthetic", expect, "src/engine", pattern)]
        case_lines, case_stale = report_lines(probes)
        check(len(case_lines) == 1,
              "the synthetic probe must produce exactly one row")
        reported_stale = bool(case_stale)
        check(reported_stale == must_be_stale,
              "expect=%s, pattern=%r: the row must be %s, but the "
              "checker reported %s"
              % (expect, pattern,
                 "stale" if must_be_stale else "ok",
                 "stale" if reported_stale else "ok"))
        # The row's own printed marker must agree with the stale list.
        marker_stale = case_lines[0].lstrip().startswith("STALE")
        check(marker_stale == reported_stale,
              "the printed marker and the stale list must agree "
              "(expect=%s, pattern=%r)" % (expect, pattern))

    # The CLOSED extraction must understand a **grouped** id cell.
    # A slice can close two items at once, and the plan writes that as
    # one row whose id cell is `**D3 + D4**`.  A guard that only
    # understood the single-id form would not see D3 and D4 as closed,
    # and so would not protect their rows -- the check that is supposed
    # to keep a landed item's row flipped to "present" would go blind
    # on exactly the items that share a slice with another.
    #
    # This is pinned against a synthetic plan, not the real one, so it
    # keeps holding whatever the real plan's rows look like.
    synthetic = (
        "| **D3 + D4** default device policy, camera + hot-plug "
        "| §1.2: \"the default policy lives in an example\" "
        "| **CLOSED.**  the slice landed "
        "| slice `v1-device-policy` (`db2dc0de`) |\n"
        "| **N1** TURN/TLS | §1.1: \"silently degraded\" "
        "| **CLOSED.**  the slice landed "
        "| slice `v1-turn-tls` |\n"
        "| **N6** the SDK sample | §1.1: \"does not exist\" "
        "| not done yet | slice `v1-sdk-sample` |\n"
    )
    closed = closed_items(synthetic)
    check(closed == {"D3", "D4", "N1"},
          "the CLOSED extraction must find D3, D4 and N1 in the "
          "synthetic plan -- a grouped id cell must yield every id in "
          "the group -- but found %s" % sorted(closed))
    check("N6" not in closed,
          "an item that is not CLOSED must not be reported as closed")

    # The table must agree with the plan.  This is the check that
    # catches the failure mode the checker exists for: an item the plan
    # declares CLOSED but whose row still asserts the *absence* of the
    # thing that closed it.
    plan = ROOT / "docs" / "plan" / "v1.4-plan.md"
    if plan.exists():
        text = plan.read_text(encoding="utf-8")
        closed = closed_items(text)
        for item in sorted(closed):
            rows = [p for p in PROBES if p[0] == item]
            check(bool(rows),
                  "the plan declares %s CLOSED, but the checker has no "
                  "row for it" % item)
            for _, _, expect, _, _ in rows:
                check(expect == "present",
                      "the plan declares %s CLOSED, so its row must "
                      "expect the feature to be 'present'; a row that "
                      "still expects 'absent' reports a permanent "
                      "false alarm" % item)

    if failures:
        print("FAIL -- the self-test caught a misclassification:")
        for f in failures:
            print("  - %s" % f)
        return 1
    print("PASS -- the self-test confirms every classification.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true",
                        help="check the checker")
    parser.add_argument("--json", action="store_true",
                        help="machine-readable output")
    parser.add_argument("--strict", action="store_true",
                        help="exit non-zero on any STALE row")
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    lines, stale = report_lines()

    if args.json:
        import json
        print(json.dumps({
            "claims": len(lines),
            "stale": len(stale),
            "rows": [ln.strip() for ln in lines],
        }, indent=2))
    else:
        print("v1.4 plan status -- %d claim(s) checked against the tree"
              % len(lines))
        for line in lines:
            print(line)
        if stale:
            print()
            print("%d claim(s) in the plan no longer hold:" % len(stale))
            for row in stale:
                print("  - %s" % row)
            print()
            print("The plan's §1 and §10 describe a tree that has moved on.")
            print("Update them (docs/plan/v1.4-plan.md) or the next reader")
            print("follows a table that is not true.")

    if stale and args.strict:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
