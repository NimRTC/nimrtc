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
    ("N2", "there are no TCP candidates", "absent",
     "src/modules/sdp", "tcptype"),
    ("N2", "there are no TCP candidates (ice)", "absent",
     "src/modules/ice", "tcptype"),

    # -- N3: the reconnection policy -----------------------------------
    # The *declaration* and the *definition* of `restart_ice()` are the
    # API, not a policy: the claim is that nothing calls it *by
    # itself*.  So the probe looks for the policy itself -- the named
    # collaborator the plan says N3 introduces.  A file that does not
    # exist is the honest way to say "no policy is here".
    ("N3", "there is no reconnection policy in the engine", "absent",
     "src/engine/src", "engine_ice_recovery"),

    # -- N4: connectivity diagnostics ----------------------------------
    ("N4", "there is no failure-reason vocabulary", "absent",
     "src/engine", "ice_failure_reason"),
    ("N4", "there is no gathering-state field", "absent",
     "src/engine", "ice_gathering_state"),
    ("N4", "there is no field naming the TURN transport", "absent",
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

    # -- N6: the SDK sample ---------------------------------------------
    ("N6", "there is no connectivity profile", "absent",
     "src", "connectivity_profile"),

    # -- N7: credentials as a seam -------------------------------------
    ("N7", "there is no credential provider", "absent",
     "src/engine", "turn_credentials"),

    # -- N8: the path change is reported -------------------------------
    ("N8", "the local recovery is not reported", "absent",
     "src/engine", "set_on_path_change|path_changed"),

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
    ("D3", "the default capture policy lives in an example", "present",
     "examples/remote-desktop", "windows-wgc|macos-screencapturekit"),

    # -- D4: hot-plug ---------------------------------------------------
    ("D4", "there is no hot-plug in the capture module", "absent",
     "src/modules/video_capture", "device_added|device_removed|hot.?plug"),

    # -- D5: DTX coupling ----------------------------------------------
    ("D5", "DTX is not coupled to concealment", "absent",
     "src/modules/audio_rx", "dtx"),

    # -- D7: the app-convenience surface -------------------------------
    ("D7", "there is no call-state callback", "absent",
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

    # The table must agree with the plan.  This is the check that
    # catches the failure mode the checker exists for: an item the plan
    # declares CLOSED but whose row still asserts the *absence* of the
    # thing that closed it.
    plan = ROOT / "docs" / "plan" / "v1.4-plan.md"
    if plan.exists():
        text = plan.read_text(encoding="utf-8")
        closed = set(re.findall(
            r"\|\s*\*\*([A-Z]\d+)\*\*[^|]*\|[^|]*\|\s*\*\*CLOSED",
            text))
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
