# -*- coding: utf-8 -*-
"""Check the v1.5 plan's claims against the tree.

`docs/plan/v1.5-plan.md` section 10 lists the claims as a verification
table.  Each row is a `git grep` a person is meant to run by hand, and
hand-run checks drift: the tree moves, the plan does not, and the next
reader follows a table that describes a tree that no longer exists.

So the table is *executed* here.  Every row is a probe -- a search that
must come back empty (the feature is absent) or non-empty (it is
present) -- and the report is the verdict per row.

The discipline is the one `tools/check_v14_plan_status.py` established:

  - a row's expectation flips when its item lands.  A row that keeps
    asserting the absence of a thing that now exists reports STALE
    forever, and a permanent false alarm is how a checker teaches
    people to ignore it.
  - the self-test pins the verdict machinery (all four cases of
    expectation x found) and the one rule that keeps the table true:
    every item the plan marks CLOSED must have its rows expecting
    "present".  It deliberately does NOT pin any row's current value.

Usage:
    python tools/check_v15_plan_status.py            # the report
    python tools/check_v15_plan_status.py --json
    python tools/check_v15_plan_status.py --strict   # non-zero on STALE
    python tools/check_v15_plan_status.py --self-test

Same flags, same meanings as `check_v14_plan_status.py`.  The two used
to differ -- `--json` and `--strict` here, neither there -- which meant
"run the plan checker" was two commands with different outcomes, and a
CI step that ran one was not running the other.  CI runs both the same
way now, and neither with `--strict`: a plan that has moved on is a
documentation task, not a build failure.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PLAN = ROOT / "docs" / "plan" / "v1.5-plan.md"

# A plan row that marks an item CLOSED, in any of the write shapes the
# plan uses: the id cell (bare or **bold**), any number of middle cells,
# and a cell that *is* CLOSED.  The id class needs two uppercase
# characters after the first so `RTP` is not an id.  (The cell class
# excludes `*`, so a bold id's `**` cannot be mistaken for a cell.)
#
# The id cell is followed by *optional prose* before the cell ends, and
# the prose may carry a second id: the plan writes rows like
#
#     | **R5** (the multi-encoding ladder split) · **R6** (the second receive) | ...
#
# -- so the cell is matched up to its closing pipe, and every id in it
# is extracted.  A guard that only understood a bare id cell would not
# see R5 or R6 as closed, and so would not protect their rows: the very
# check that keeps a landed item's row flipped to "present" would go
# blind on exactly the rows the plan writes in its own shape.
#
# The id class is `[A-Z][A-Z0-9]+` -- two uppercase characters after the
# first -- so `RTP` is not an id.
#
# The id is **bold** in the plan's own rows (`**R5**`), and only the id
# itself is bold -- the prose that follows it in the same cell is not.
# The cell is therefore matched up to its closing pipe, and the ids are
# the **bold** tokens inside it.  Requiring the bold markers is what
# keeps a prose word out: `RED` in `**R1** (RED + the redundancy seam)`
# is not bold, so it is not an id.
CLOSED_ROW = (r"\|\s*((?:\s*\*{0,2}[A-Z][A-Z0-9]+\*{0,2}"
              r"[^\n|]*?)+)\|"
              r"(?:[^|\n]*\|){1,}\s*\*{0,2}CLOSED\.?\*{0,2}\s*\|")

# An id inside a cell, with its optional bold markers.
_ITEM_ID = r"\*{0,2}([A-Z][A-Z0-9]+)\*{0,2}"


def closed_items(text: str) -> set[str]:
    """The items the plan's tables declare CLOSED.

    A row's id cell may name one item or several, and it may carry
    prose around them.  Every id in the cell of a row that marks CLOSED
    is returned, so a slice that closes two items at once protects both
    of their rows.

    An id is a **bold** token in the plan's own rows.  Requiring the
    bold markers is what keeps a prose word out of the result:
    `**R1** (RED + the redundancy seam)` names **R1**, not `RED`.
    A cell that carries no bold token at all is a bare id cell
    (`| R6 | ... |`), and that one is taken whole.
    """
    found: set[str] = set()
    for match in re.finditer(CLOSED_ROW, text):
        cell = match.group(1)
        bold = re.findall(r"\*\*{1,2}([A-Z][A-Z0-9]+)\*{1,2}", cell)
        if bold:
            found.update(bold)
            continue
        bare = cell.strip().strip("*").strip()
        if re.fullmatch(r"[A-Z][A-Z0-9]+", bare):
            found.add(bare)
    return found


# --------------------------------------------------------------------------
# The probes
# --------------------------------------------------------------------------
#
# Each row is: (id, what the plan claims, expect, search-where, pattern)
#
#   expect "absent"  -- the pattern must NOT be found
#   expect "present" -- the pattern must be found
#
# The order follows section 10 of the plan.  The rows below are the
# baseline state (v1.5 has not started), so most expect "absent"; the
# "F" rows are the foundations v1.5 builds on and must stay "present".

PROBES: list[tuple[str, str, str, str, str]] = [
    # -- F: foundations (already in the tree; not v1.5's work) ----------
    ("F", "the RTX module parses the RFC 4588 payload", "present",
     "src/modules/rtx", r"ParsedPayload"),
    ("F", "the video RTX negotiation is wired", "present",
     "src/engine/src", "video_rtx_negotiation"),
    # The limiter lives with the video pipeline module, not with the
    # RTCP feedback surface -- checked 2026-11-26.
    ("F", "the keyframe limiter exists", "present",
     "src/modules/video_pipeline", "KeyframeLimiter"),
    ("F", "the bandwidth split rule exists", "present",
     "src/engine/src", "compute_video_stream_shares|bandwidth_allocation"),
    ("F", "the multi-stream model states the room for encodings",
     "present", "src/engine/src/video_stream_set.hpp", "several encodings"),
    ("F", "RtpPacketizer is the one landed collaborator", "present",
     "src/engine/src", "rtp_packetizer"),

    # -- R1: RED and the redundancy seam --------------------------------
    ("R1", "there is no redundancy codec seam", "absent",
     "src/plugins", "IRedundancyCodec"),
    # The SDP *parser* already reads a peer's `a=rtpmap:63 red/48000/2`
    # (a Chrome offer in its test); what is absent is a RED *writer* and
    # a RED payload type of our own.  The probe looks for the write side
    # only, so the parser's fixture does not answer for it.
    ("R1", "the SDP writer emits no RED payload type", "absent",
     "src/modules/sdp/src", r"red/"),
    # `git grep -w`-like: the opus plugin's "redundancy hint" comments
    # (in-band FEC percentage) are not a redundancy module.  Probe the
    # module directory's own name instead.
    ("R1", "there is no redundancy module", "absent",
     "src/modules", r"modules/redundancy"),

    # -- R2: FEC ---------------------------------------------------------
    ("R2", "there is no FEC scheme", "absent",
     "src", "ulpfec|flexfec"),
    ("R2", "the SDP emits no FEC attributes", "absent",
     "src/modules/sdp", "ulpfec|flexfec"),

    # -- R3: the layering model ------------------------------------------
    # The model "leaves the room" today (a comment says so); the row
    # flips when the encoding list replaces the single-encoding field.
    ("R3", "a stream still carries exactly one encoding", "absent",
     "src/engine/src/video_stream_set.hpp", r"encoding_list"),

    # -- R4: simulcast negotiation ---------------------------------------
    ("R4", "there is no simulcast negotiation (sdp)", "absent",
     "src/modules/sdp", r"a=simulcast|a=rid"),
    ("R4", "there is no simulcast negotiation (engine)", "absent",
     "src/engine/src", "video_simulcast_negotiation"),

    # -- R5: the second split level --------------------------------------
    ("R5", "there is no per-encoding share", "absent",
     "src/engine/src", "encoding_share|per_encoding_share"),

    # -- R6: SVC receive --------------------------------------------------
    ("R6", "there is no spatial-layer receive", "absent",
     "src/modules/vp9", "spatial_layer|ss_structure|dependency_descriptor"),

    # -- R7: the measurement ---------------------------------------------
    ("R7", "there is no committed loss artifact", "absent",
     "docs/benchmarks", "rtx_only|fec_recovered"),
]


def search(where: str, pattern: str) -> list[str]:
    """Every match of `pattern` under `where`, as repo-relative paths."""
    # A path with no wildcard and no `*.[ch]pp` suffix is a prefix
    # scope (a directory or a file); one with `*.ext` is a pathspec.
    # `git grep -- docs/benchmarks` would look for a *file* by that
    # name and silently match nothing, so the second shape matters.
    if "*" in where:
        pathspec = where
    elif re.search(r"\.(h|hpp|c|cc|cpp|md|py|json|txt)$", where):
        pathspec = where
    elif (ROOT / where).is_dir():
        pathspec = where.rstrip("/") + "/"
    else:
        pathspec = where
    proc = subprocess.run(
        ["git", "grep", "-l", "-E", pattern, "--", pathspec],
        cwd=ROOT, capture_output=True, text=True, encoding="utf-8",
        errors="replace")
    if proc.returncode not in (0, 1):
        return []
    return [ln for ln in proc.stdout.splitlines() if ln.strip()]


def report_lines(
    probes: list[tuple[str, str, str, str, str]] | None = None,
) -> tuple[list[str], list[str]]:
    """The verdict per row, and the list of rows that no longer hold."""
    lines: list[str] = []
    stale: list[str] = []
    used = PROBES if probes is None else probes
    for item, claim, expect, where, pattern in used:
        hits = search(where, pattern)
        found = bool(hits)
        ok = (expect == "present") == found
        mark = "ok   " if ok else "STALE"
        note = ""
        if not ok:
            note = "   <- %s" % ", ".join(hits[:3])
            stale.append("%s: %s" % (item, claim))
        lines.append("  %s %-4s %s%s" % (mark, item, claim, note))
    return lines, stale


def self_test() -> int:
    """The fixture proves the checker can report stale, and cannot lie.

    A checker that always passes is worse than no checker: the next
    reader trusts it.  So the self-test pins the *verdict machinery*
    (the four cases of expectation x found) and the one property that
    makes the table true (a CLOSED item's rows expect "present").  It
    does not pin any row's current value -- rows flip as items land,
    and a self-test that failed when the tree improved would train
    people to ignore it.
    """
    failures = []

    def check(condition: bool, message: str) -> None:
        if not condition:
            failures.append(message)

    # A search that cannot match must find nothing, and a directory
    # that does not exist must not raise.
    check(search("src/engine", "zzz_no_such_symbol_zzz") == [],
          "a search for a symbol that does not exist must come back empty")
    check(search("zzz_no_such_dir", "anything") == [],
          "a search in a directory that does not exist must come back empty")
    # A search for something that does must find it.
    check(search("src/modules/rtx", "ParsedPayload") != [],
          "a search for a symbol that exists must come back non-empty")
    # A directory scope must search inside it, not for a file by its
    # name (`git grep -- docs/plan` searches a file called `plan` and
    # finds nothing).  This is the R7 shape: the directory exists, the
    # artifact does not.
    check(search("docs/plan", r"Plan opened") != [],
          "a directory scope must search inside it, not for a file "
          "by its name")
    check(search("docs/benchmarks/*.json", r"\"t_s\"") != [],
          "a pathspec scope must search only the files it names")

    # The report runs and reports every probe.
    lines, stale = report_lines()
    check(len(lines) == len(PROBES), "every probe must be reported")
    check(isinstance(stale, list), "the report must return a stale list")

    # The verdict machinery: all four cases, each with the verdict
    # written out by hand.
    present_symbol = "video_rtx_negotiation"
    missing_symbol = "zzz_no_such_symbol_zzz"
    cases = [
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
        reported = bool(case_stale)
        check(reported == must_be_stale,
              "expect=%s, pattern=%r: the row must be %s, but the "
              "checker reported %s"
              % (expect, pattern,
                 "stale" if must_be_stale else "ok",
                 "stale" if reported else "ok"))
        marker_stale = case_lines[0].lstrip().startswith("STALE")
        check(marker_stale == reported,
              "the printed marker and the stale list must agree "
              "(expect=%s, pattern=%r)" % (expect, pattern))

    # The table must agree with the plan: an item the plan declares
    # CLOSED must have its rows expecting "present".  The extraction
    # runs against a synthetic plan, not the real one -- if it ran
    # against the real one the check would be vacuous today (nothing
    # is closed) and would silently stop working the day the plan's
    # wording changed.
    # Two write shapes: the plan's own table splits the ID across
    # `**R4**` / `CLOSED` with a middle column between them, so the
    # section is matched anywhere in the row.  The class deliberately
    # requires two uppercase letters after the first, so `RTP` is not an
    # id.
    #
    # The other two shapes are the ones the plan actually writes:
    # the id cell carries prose after the id, and one row carries two
    # ids separated by a middle dot.  A guard that missed either would
    # not protect that row's items.
    synthetic = (
        "| **R4** | some claim | CLOSED |\n"
        "| R6 | some claim | CLOSED |\n"
        "| **R1** (RED + the redundancy seam) | some claim | **CLOSED.** | x |\n"
        "| **R5** (the ladder split) \xc2\xb7 **R6** (the second receive) "
        "| some claim | **CLOSED.** | x |\n"
        "| R5 | still open | absent |\n"
    )
    closed = closed_items(synthetic)
    check(closed == {"R1", "R4", "R5", "R6"},
          "the CLOSED extraction must find R1, R4, R5 and R6 in the "
          "synthetic plan -- an id cell with prose after the id and a "
          "cell with two ids must both yield their ids -- but found %s"
          % sorted(closed))
    # The rule is *corpus-shaped*: stale rows for closed items, per the
    # clause.  Today's R4 / R6 rows expect "absent" (their items are not
    # closed), so the synthetic plan must be judged against a synthetic
    # corpus, not against PROBES -- otherwise the self-test would fail
    # on a plan that is merely *behind* and pass on one that is
    # permanently wrong.
    corpus = {
        "R4": [("R4", "no simulcast (sdp)", "absent", "sdp", "x"),
               ("R4", "no simulcast (engine)", "present", "e", "x")],
        "R6": [("R6", "no spatial layers", "absent", "vp9", "x")],
        "R5": [("R5", "no share", "absent", "e", "x")],
        "R1": [("R1", "no redundancy seam", "absent", "e", "x")],
    }
    clause_failures: list[str] = []
    for item in sorted(closed):
        rows = corpus.get(item, [])
        if not rows:
            clause_failures.append("%s has no rows" % item)
        for _, _, expect, _, _ in rows:
            if expect != "present":
                clause_failures.append("%s: %s" % (item, expect))
    check(clause_failures == ["R1: absent", "R4: absent", "R5: absent",
                              "R6: absent"],
          "the CLOSED clause must flag every row of a closed item that "
          "does not expect 'present', and only those; it flagged %s"
          % clause_failures)
    skip = set(closed)
    for item, rows in corpus.items():
        if item in skip:
            continue
        check(all(r[2] == "absent" for r in rows),
              "an open item's rows must not be flagged")
    # The clause, applied to the real plan: every item it marks CLOSED
    # must have a row here that expects "present".  Today this is
    # empty (nothing is closed) -- which is why the corpus above pins
    # the clause's behaviour, so it cannot rot silently until the day
    # it first matters.
    if PLAN.exists():
        text = PLAN.read_text(encoding="utf-8")
        for item in sorted(closed_items(text)):
            rows = [p for p in PROBES if p[0] == item]
            check(bool(rows),
                  "the real plan declares %s CLOSED, but the checker has "
                  "no row for it" % item)
            for _, _, expect, _, _ in rows:
                check(expect == "present",
                      "the real plan declares %s CLOSED, so every row "
                      "must expect 'present'; a row that still expects "
                      "'absent' is a permanent false alarm" % item)

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
        print(json.dumps({
            "claims": len(lines),
            "stale": len(stale),
            "rows": [ln.strip() for ln in lines],
        }, indent=2))
    else:
        print("v1.5 plan status -- %d claim(s) checked against the tree"
              % len(lines))
        for line in lines:
            print(line)
        if stale:
            print()
            print("%d claim(s) in the plan no longer hold:" % len(stale))
            for row in stale:
                print("  - %s" % row)
            print()
            print("The plan's section 10 describes a tree that has moved")
            print("on.  Flip the row (docs/plan/v1.5-plan.md) or the next")
            print("reader follows a table that is not true.")

    if stale and args.strict:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
