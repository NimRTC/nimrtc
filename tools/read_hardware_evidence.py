#!/usr/bin/env python3
"""Read the hardware-codec evidence, from this checkout or from GitHub.

`scripts/check-release-freeze.sh` needs one fact at tag-cut time: is
there a PASS hardware-encode record (v1.3 plan Q7) measured on the tree
being tagged?

It used to answer that from `build/hardware/*.jsonl` alone -- files the
`hardware.yml` workflow writes on a *self-hosted GPU runner*, which is
not the machine a release is cut on.  So the v1.4.0 final-tag preflight
was red by construction: the evidence existed, but not in the checkout
that ran the check.

This script reads the two places the evidence actually lives:

  1. `build/hardware/*.jsonl` in the checkout, when the check is run on
     the GPU runner itself (or with an archived copy dropped there);
  2. otherwise the **archived run**: `tools/run_hardware_job.py` writes
     the record into the step summary, into the job outputs, and into a
     **check-run annotation** (the workflow-file `::error::` format).
     The annotation is the one the REST API serves through the
     `actions:read` scope, and it is read here from
     `GET /actions/runs/{id}/jobs` -- the annotation is on the job the
     record belongs to.  The job log (where the summary and the outputs
     live) is *not* used: its download endpoint is a different service
     that answers a bare 404 to a token without `actions:read`, which
     reads exactly like "the log has expired".

     The newest successful run of `hardware.yml` on the commit being
     tagged is tried first, then the newest successful run on any
     commit (it is then reported as another tree, not counted).

It is a separate script rather than a section of the shell for one
reason: the record is JSONL inside a fenced block inside a log that
also carries the runner's line prefixes, and pulling that apart with
`sed` is the kind of parsing that works until the day it silently does
not (it did: the first attempt fetched the log and then could not find
a record in it).  Python's `json` is the right tool, and this tree
already requires Python 3 for every other tool in `tools/`.

Output: one `key=value` line per result, so a POSIX shell can read it
without a JSON parser:

    source=<local|remote|none>
    detail=<where the record came from, for the report line>
    run=<run id, for remote>
    commit=<git commit the newest PASS record names, or empty>
    date=<date of that record>
    dirty=<yes|no|unknown>
    pass=<0|1>              whether a PASS record was found

Exit code 0 when the script ran (whatever it found -- `pass=0` is an
answer, not an error), 1 when no source could be read at all *and*
`--require-source` was passed (no gh, no token, no successful run, an
expired log).
"""

from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOCAL_DIR = ROOT / "build" / "hardware"
REPO = "NimRTC/nimrtc"
SCHEMA = "nimrtc-hw-e2e-evidence/1"

# The record is written as a fenced block: ```` ```jsonl ```` ... ```` ``` ````.
FENCE_RE = re.compile(r"```jsonl\n(.*?)```", re.DOTALL)
# The job log prefixes every line with its timestamp ("2026-11-24T19:5
# 2:55.1234567Z ").  The prefix is stripped before the block is parsed;
# a line that does not start with a timestamp is left as it is.
LOG_PREFIX_RE = re.compile(r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d+Z ")


def gh(args: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(["gh", "api"] + args, capture_output=True,
                          text=True, encoding="utf-8", errors="replace")


def local_records(directory: Path) -> list[dict]:
    records: list[dict] = []
    if not directory.is_dir():
        return records
    for path in sorted(directory.glob("*.jsonl")):
        for line in path.read_text(encoding="utf-8",
                                   errors="replace").splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                entry = json.loads(line)
            except json.JSONDecodeError:
                continue
            if entry.get("schema") == SCHEMA:
                records.append(entry)
    return records


# The annotation the runner emits for a line `::error title=...,file=...::msg`.
# GitHub stores the *displayed* message, which is the `title` and the
# message joined; the record is the message, so the title is peeled off.
ANNOTATION_TITLE = "nimrtc hardware evidence: "


def annotation_records(jobs: list[dict]) -> list[dict]:
    """The records carried by the job's check-run annotations."""
    records: list[dict] = []
    for job in jobs:
        for ann in job.get("annotations") or []:
            text = ann.get("message", "")
            if text.startswith(ANNOTATION_TITLE):
                text = text[len(ANNOTATION_TITLE):]
            text = text.strip()
            if not text.startswith("{"):
                continue
            try:
                entry = json.loads(text)
            except json.JSONDecodeError:
                continue
            if entry.get("schema") == SCHEMA:
                records.append(entry)
    return records


def record_block(log_text: str) -> list[dict]:
    """The evidence block inside a job log, with the log's prefixes off."""
    stripped = "\n".join(LOG_PREFIX_RE.sub("", line)
                         for line in log_text.splitlines())
    records: list[dict] = []
    for block in FENCE_RE.findall(stripped):
        for line in block.splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                entry = json.loads(line)
            except json.JSONDecodeError:
                continue
            if entry.get("schema") == SCHEMA:
                records.append(entry)
    return records


def remote_records(candidate: str) -> tuple[list[dict], str, str]:
    """(records, detail, run id) from the archived run, or ([], reason, "")."""
    runs = gh(["repos/%s/actions/workflows/hardware.yml/runs?per_page=30"
               % REPO])
    if runs.returncode != 0:
        return [], "gh could not list the hardware runs (auth or network)", ""
    try:
        data = json.loads(runs.stdout)
    except json.JSONDecodeError:
        return [], "gh returned an unparseable run list", ""
    ok = [r for r in data.get("workflow_runs", [])
          if r.get("conclusion") == "success"]
    if not ok:
        return [], "no successful hardware run", ""
    chosen = next((r for r in ok if r.get("head_sha") == candidate), ok[0])
    run_id = str(chosen["id"])
    # The annotations come with the job objects of *this* run, and are
    # served by the same endpoint -- one call, and it is the one the
    # `actions:read` scope grants.
    runs_api = gh(["repos/%s/actions/runs/%s/jobs?per_page=100"
                   % (REPO, run_id)])
    if runs_api.returncode != 0:
        return [], "run %s: the job list could not be read" % run_id, ""
    try:
        jobs = json.loads(runs_api.stdout).get("jobs", [])
    except json.JSONDecodeError:
        return [], "run %s: the job list is unparseable" % run_id, ""
    records = annotation_records(jobs)
    if not records:
        return [], ("run %s: no evidence annotation (the run predates "
                    "the annotation copy, or the job failed before the "
                    "recorder)" % run_id), ""
    return records, "the hardware workflow's run %s" % run_id, run_id


def self_test() -> int:
    import tempfile
    failures: list[str] = []

    def check(cond: bool, msg: str) -> None:
        if not cond:
            failures.append(msg)

    record = ('{"schema": "nimrtc-hw-e2e-evidence/1", "status": "PASS", '
              '"git_commit": "%s", "date": "2026-11-25", '
              '"git_dirty": false}') % ("a" * 40)
    # 1. The annotation form, as the runner stores it (title + message).
    jobs = [{"annotations": [
        {"message": ANNOTATION_TITLE + record}]}]
    found = annotation_records(jobs)
    check(len(found) == 1, "an annotation carrying the record is read")
    check(found and found[0]["status"] == "PASS",
          "the annotation's record keeps its status")
    # 2. The raw form (no title): GitHub may store only the message.
    raw = [{"annotations": [{"message": record}]}]
    check(len(annotation_records(raw)) == 1, "a raw annotation is read too")
    # 3. A log-prefixed fenced block is still readable (the fallback).
    block = "2026-11-24T19:52:55.1234567Z ##[group]x\n" + \
            "2026-11-24T19:52:55.1234567Z ```jsonl\n" + \
            "2026-11-24T19:52:55.1234567Z " + record + "\n" + \
            "2026-11-24T19:52:55.1234567Z ```\n"
    check(len(record_block(block)) == 1, "a fenced block in a log is read")
    # 4. Noise is not a record.
    check(annotation_records([{"annotations": [{"message": "hello"}]}]) == [],
          "an ordinary annotation is not an evidence record")
    check(annotation_records([]) == [], "no jobs, no records")
    # 5. The newest PASS wins, and a record without a commit does not count.
    older = dict(found[0], date="2026-11-01") if found else {}
    newer = dict(found[0], date="2026-11-26",
                 git_commit="b" * 40) if found else {}
    no_commit = dict(found[0], git_commit="") if found else {}
    pick = newest_pass([older, newer, no_commit])
    check(pick is not None and pick["git_commit"] == "b" * 40,
          "the newest PASS record with a commit is chosen")
    if failures:
        print("FAIL -- the self-test caught a misclassification:")
        for f in failures:
            print("  - " + f)
        return 1
    print("PASS -- the self-test confirms every classification.")
    return 0


def newest_pass(records: list[dict]) -> dict | None:
    passes = [e for e in records if e.get("status") == "PASS"
              and e.get("git_commit")]
    if not passes:
        return None
    passes.sort(key=lambda e: (e.get("date", ""), e.get("timestamp_utc", "")))
    return passes[-1]


def emit(**fields) -> None:
    for key, value in fields.items():
        print("%s=%s" % (key, value))


def main(argv: list[str]) -> int:
    candidate = ""
    offline = False
    require_source = False
    for arg in argv[1:]:
        if arg == "--self-test":
            return self_test()
        if arg == "--offline":
            offline = True
        elif arg == "--require-source":
            require_source = True
        elif not arg.startswith("-"):
            candidate = arg

    records = local_records(LOCAL_DIR)
    source, detail, run_id = "local", "build/hardware", ""
    if not records and not offline:
        records, detail, run_id = remote_records(candidate)
        if records:
            source = "remote"

    if not records:
        emit(source="none",
             detail=detail if not offline
             else "no local record (--offline)",
             run="", commit="", date="", dirty="unknown", **{"pass": 0})
        return 1 if require_source else 0

    entry = newest_pass(records)
    if entry is None:
        emit(source=source, detail=detail, run=run_id,
             commit="", date="", dirty="unknown", **{"pass": 0})
        return 0
    emit(source=source, detail=detail, run=run_id,
         commit=entry.get("git_commit", ""),
         date=entry.get("date", ""),
         dirty="yes" if entry.get("git_dirty") else "no",
         **{"pass": 1})
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
