#!/usr/bin/env python3
"""ci_retry_runner.py -- re-run the CI jobs that were starved of a runner.

## The problem

A GitHub-hosted runner is not guaranteed.  A job that asks for a
saturated pool (the `macos-15` arm64 image is the chronic case in this
repository) can sit in the queue until the platform gives up:

    The job was not acquired by Runner of type hosted even after
    multiple attempts

and end up `cancelled` with **no steps** and **no runner**.  The run's
conclusion is then `failure`, and the merge gate -- which requires that
check by name -- is red for a reason that has nothing to do with the
tree.

Observed repeatedly in this repository (2026-11-16): every one of
PRs #65, #66 and #67 needed a manual `gh run rerun` before the macOS
leg would go green.  The rerun itself is the correct remedy, but doing
it by hand makes CI unattended only for the runs that happen to be
watched.

## The danger this script has to avoid

Retrying blindly is *worse* than not retrying at all: a retry that
re-runs a **real** failure hides the defect, and the defect is exactly
what the CI run was for.

So the script classifies a job before it touches it, and the
classification is the whole point:

    starved        the job never got a runner -- no steps ran, and the
                   runner fields are empty.  Retrying this is right.

    genuine        the job ran and failed.  Retrying this hides a bug.
                   The script refuses.

    fine           the job succeeded, or it was skipped, or it was
                   cancelled in a way that is not runner starvation
                   (a concurrency cancel that *did* get a runner).

The two signatures are disjoint and have been checked against real
runs of this repository:

    run 43938323684  macos-clang / arm64   steps=0   runner_name=""   -> starved
    run 43877514623  macos-clang / arm64   steps=19  runner_name set -> genuine
    run 43864579652  linux-sanitizers / asan-ubsan  steps=11, runner set -> genuine

A job that *ran* and failed is never retried, whatever the failure
looks like.

`cancelled` is squarely in scope.  Measured over 60 cancelled runs of
this repository (2026-11-19):

    steps>0, runner assigned      124 jobs   a live job killed with the run
    steps==0, no runner            25 jobs   never reached a machine

The first kind is a *concurrency cancel* or a manual stop: the group in
`ci.yml` is `cancel-in-progress: true`, so pushing again cancels the
run in flight, and those jobs are `fine`.  The second kind is the
starved shape, and re-running it is the remedy.  (`failure` with no
steps and no runner is the same shape: the platform gave up before the
job could start.)

## The 403 that made this script red (the 2026-11-18 finding)

Nine of the ten red `ci-retry` runs between 2026-11-17 and 2026-11-19
failed with

    ERROR: rerunning ... failed: 403 Forbidden

and one with `HTTP Error 502: Bad Gateway`.  Neither is a
misclassification.  The 403 is a **race**: `workflow_run` fires when
the *run* transitions, but a concurrency cancel tears jobs down
asynchronously, and the target run's state is a snapshot taken at a
different moment than its jobs'.  Concretely, run 44439751037 was
still `in_progress` when its `ci-retry` run fired at 01:49:22Z and
tried to re-run attempt-1 jobs -- the platform refuses to re-run a
job whose run is still running.  Retrying the API call does not help;
the answer is to wait until the run is genuinely `completed`.

The 502 is a plain platform hiccup.  It *is* retryable: the script now
retries 5xx and 429 with exponential backoff, and gives up with a
clear message otherwise.

## The loop guard

A retry can itself be starved.  The guard used to be the run's
`run_attempt`, which was wrong in both directions: a job-level re-run
increments the counter for *every* job in the run, so a run whose
attempt 2 had one fresh starved job (run 44306330102: the macOS leg
starved in both attempts) was never re-retried.  Now the guard is the
job that is actually being retried: the **same job name** is allowed
at most `MAX_RETRIES` re-runs across the run's attempts, counted by
walking the per-attempt job lists.  A job that starts running on its
second try stops being starved and needs no further bookkeeping.

## Usage

    python tools/ci_retry_runner.py --repo owner/name --run-id 12345
    python tools/ci_retry_runner.py --repo owner/name --run-id 12345 --dry-run
    python tools/ci_retry_runner.py --self-test

The token comes from `GITHUB_TOKEN` (or `GH_TOKEN`).

Exit code 0 when the run needed no retry or the retry was requested
successfully, 1 on an unrecoverable API error.  Exit code 0 also when
there is simply nothing to do; a guard that fails on the quiet path
would be noise of its own.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request

API_ROOT = "https://api.github.com"
API_VERSION = "2022-11-28"

# The conclusions a job can end on.  `cancelled` is included because a
# starved job is reported as cancelled, not as failure.
CONCLUSIONS = ("failure", "cancelled")

# HTTP statuses that mean "the platform wobbled", not "you asked for
# something you may not have".  403 is deliberately NOT in this set: in
# this repository it is the run-still-in-progress race, and retrying the
# same call would get the same 403.
RETRYABLE_HTTP = frozenset([429, 500, 502, 503, 504])


# ---------------------------------------------------------------------------
# The classification
# ---------------------------------------------------------------------------

def classify_job(job: dict) -> str:
    """Classify one job as "starved", "genuine" or "fine".

    The rule is deliberately narrow.  A job is *starved* only when
    **no step ran at all** and **no runner was ever assigned**:

      * `steps` empty -- the platform could not start the job, so the
        build never began.  A job that ran a step and then failed ran
        the code, and its failure is the code's.
      * `runner_name` empty -- the job never reached a machine.

    Both conditions are required.  A job with an empty `steps` list but
    a runner name is a job that was cancelled before its first step
    started (a concurrency cancel, a superseded run), and re-running it
    would resurrect a run the repository has already moved past.

    A job that has *no* `steps` key at all is treated as unknown and
    left alone: an API response that omits the field is not evidence of
    starvation.
    """
    conclusion = job.get("conclusion")
    if conclusion not in CONCLUSIONS:
        return "fine"

    steps = job.get("steps")
    if steps is None:
        # The API omitted the field: not evidence of starvation.
        return "fine"
    if len(steps) != 0:
        # The job ran.  Its failure is the code's, not the platform's.
        return "genuine"

    if job.get("runner_name"):
        # A runner was assigned, so the job was cancelled *after* being
        # scheduled -- a superseded run, not a starved queue.
        return "fine"

    # No step ran and no runner was ever assigned: the platform could
    # not hand this job a machine.
    return "starved"


def select_starved(jobs: list) -> list:
    """The starved jobs in a run."""
    return [j for j in jobs if classify_job(j) == "starved"]


def retries_spent(attempt_jobs: list) -> dict:
    """How many times each job *name* has been re-run so far.

    Each attempt's job list is one snapshot.  A name that appears in
    more than one snapshot has been re-run; `len - 1` re-runs is what
    the next attempt would cost.

    A name that appears at all in an earlier attempt and is starved
    again in the latest one has already been given its chance, and
    giving it another would be the sustained-outage case the old
    `run_attempt` guard was trying to catch -- but without punishing
    every *other* job in the run for it.
    """
    seen = {}
    for jobs in attempt_jobs:
        for job in jobs:
            seen.setdefault(job.get("name"), []).append(job)
    return {name: len(snapshots) - 1 for name, snapshots in seen.items()}


def over_retry_budget(name: str, spent: dict, max_retries: int) -> bool:
    """True when `name` has no re-run left.

    `spent` is how many times the name has *already* been re-run.  The
    oldest attempt is the original, so a name that has been re-run once
    (`spent == 1`) still has one re-run in it when the budget is 1: the
    run restarted it once and it starved again, and that second
    starvation is the one worth one more try before declaring an
    outage.  Only `spent > max_retries` means the budget is gone.

    The off-by-one is the bug the old run-level guard had in the other
    direction: it treated *any* attempt above 1 as exhausted, so the
    second starvation on run 44306330102 was never retried and the
    merge gate stayed red.
    """
    return spent.get(name, 0) > max_retries


# ---------------------------------------------------------------------------
# The GitHub API
# ---------------------------------------------------------------------------

class Retryable(Exception):
    """A platform wobble: worth trying again after a pause."""


class Fatal(Exception):
    """A request that will not get a different answer if repeated."""


def _request(method: str, url: str, token: str, payload=None,
             retries: int = 4, sleep=time.sleep):
    data = None
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")

    last = None
    for attempt in range(retries + 1):
        request = urllib.request.Request(url, data=data, method=method)
        request.add_header("Authorization", "Bearer %s" % token)
        request.add_header("Accept", "application/vnd.github+json")
        request.add_header("X-GitHub-Api-Version", API_VERSION)
        if data is not None:
            request.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                body = response.read()
            if not body:
                return {}
            return json.loads(body.decode("utf-8"))
        except urllib.error.HTTPError as error:
            detail = error.read().decode("utf-8", "replace")[:300]
            if error.code in RETRYABLE_HTTP and attempt < retries:
                last = "%s %s" % (error.code, detail)
                sleep(min(2 ** attempt, 30))
                continue
            raise Fatal("%s %s %s" % (method, error.code, detail))
        except urllib.error.URLError as error:
            # A transport-level failure (DNS, reset) is a wobble too.
            if attempt < retries:
                last = str(error.reason)
                sleep(min(2 ** attempt, 30))
                continue
            raise Fatal("%s unreachable: %s" % (method, error.reason))
        except (TimeoutError, ConnectionError) as error:
            if attempt < retries:
                last = str(error)
                sleep(min(2 ** attempt, 30))
                continue
            raise Fatal("%s timed out: %s" % (method, error))
    raise Fatal("%s: retried %d times, last error: %s"
                % (method, retries, last))


def fetch_run_jobs(repo: str, run_id: int, token: str, attempt: int,
                   retries: int = 4) -> list:
    """Every job of one attempt of a run, across pages."""
    jobs = []
    page = 1
    while True:
        url = ("%s/repos/%s/actions/runs/%d/attempts/%d/jobs"
               "?per_page=100&page=%d"
               % (API_ROOT, repo, run_id, attempt, page))
        body = _request("GET", url, token, retries=retries)
        batch = body.get("jobs") or []
        jobs.extend(batch)
        if len(batch) < 100:
            break
        page += 1
    return jobs


def fetch_run(repo: str, run_id: int, token: str, retries: int = 4) -> dict:
    url = "%s/repos/%s/actions/runs/%d" % (API_ROOT, repo, run_id)
    return _request("GET", url, token, retries=retries)


def rerun_job(repo: str, job_id: int, token: str, retries: int = 4) -> None:
    url = "%s/repos/%s/actions/jobs/%d/rerun" % (API_ROOT, repo, job_id)
    _request("POST", url, token, retries=retries)


# ---------------------------------------------------------------------------
# Waiting for the run to settle
# ---------------------------------------------------------------------------

def wait_until_completed(repo: str, run_id: int, token: str,
                         polls: int, interval: float, sleep=time.sleep,
                         log=print) -> dict:
    """Poll until the run reports `status == "completed"`.

    `workflow_run` can fire while the run is still tearing down.  Re-run
    endpoints answer 403 until the run is complete, so this is the
    difference between a retry and a red run.  Verified on
    2026-11-19: run 44439751037 was still `in_progress` when its
    `ci-retry` fired.
    """
    run = fetch_run(repo, run_id, token)
    for n in range(max(0, polls)):
        if run.get("status") == "completed":
            return run
        log("run %d is still %r -- waiting (poll %d/%d)"
            % (run_id, run.get("status"), n + 1, polls))
        sleep(interval)
        run = fetch_run(repo, run_id, token)
    return run


# ---------------------------------------------------------------------------
# The action
# ---------------------------------------------------------------------------

def process(repo: str, run_id: int, token: str, dry_run: bool,
            max_retries: int, wait_polls: int = 20,
            wait_interval: float = 15.0, sleep=time.sleep,
            log=print) -> int:
    run = wait_until_completed(repo, run_id, token, wait_polls,
                               wait_interval, sleep=sleep, log=log)
    attempt = run.get("run_attempt") or 1

    if run.get("status") != "completed":
        log("run %d is still %r after waiting -- the platform has not "
            "settled it, so a re-run would be refused (403).  Leaving it "
            "to the next trigger." % (run_id, run.get("status")))
        return 0

    jobs = fetch_run_jobs(repo, run_id, token, attempt)
    attempt_jobs = [fetch_run_jobs(repo, run_id, token, a)
                    for a in range(1, attempt)] + [jobs]
    spent = retries_spent(attempt_jobs)

    log("run %d (%s) attempt %d -- %s"
        % (run_id, run.get("name"), attempt, run.get("conclusion")))

    for job in jobs:
        verdict = classify_job(job)
        marker = {"starved": "STARVED", "genuine": "GENUINE",
                  "fine": "ok     "}[verdict]
        log("  %-8s %-34s conclusion=%-9s steps=%s runner=%s retries=%d"
            % (marker, job.get("name"), job.get("conclusion"),
               len(job.get("steps") or []),
               job.get("runner_name") or "<none>",
               spent.get(job.get("name"), 0)))

    starved = select_starved(jobs)
    if not starved:
        log("nothing to retry: no job was starved of a runner")
        return 0

    eligible, exhausted = [], []
    for job in starved:
        if over_retry_budget(job.get("name"), spent, max_retries):
            exhausted.append(job)
        else:
            eligible.append(job)

    if exhausted:
        log("not retried (already re-run %d time(s) and starved again -- "
            "the platform is having a sustained outage and more retries "
            "would only burn runner minutes): %s"
            % (max_retries, ", ".join(j.get("name") for j in exhausted)))
    if not eligible:
        return 0

    failures = 0
    for job in eligible:
        name = job.get("name")
        job_id = job.get("id")
        if dry_run:
            log("would rerun %s (job %d)" % (name, job_id))
            continue
        try:
            rerun_job(repo, job_id, token)
        except Fatal as error:
            failures += 1
            log("ERROR: rerunning %s (job %d) failed: %s"
                % (name, job_id, error))
            continue
        log("reran %s (job %d) -- it had been starved of a runner"
            % (name, job_id))
    return 1 if failures else 0


# ---------------------------------------------------------------------------
# The self-test
# ---------------------------------------------------------------------------

def _job(name="macos-clang / arm64", conclusion="failure", steps=None,
         runner=""):
    if steps is None:
        steps = []
    return {
        "id": 133564450647,
        "name": name,
        "status": "completed",
        "conclusion": conclusion,
        "steps": steps,
        "runner_name": runner,
        "runner_id": 1000004908 if runner else 0,
    }


def _run(status="completed", attempt=1, conclusion="failure"):
    return {
        "id": 44439751037,
        "name": "ci",
        "status": status,
        "conclusion": conclusion,
        "run_attempt": attempt,
    }


def self_test() -> int:
    failures = []

    def expect(condition: bool, message: str) -> None:
        if not condition:
            failures.append(message)

    # --- The starved signature ------------------------------------------
    # No step ran and no runner was assigned: the platform could not
    # hand the job a machine.  This is the only shape we retry.
    expect(classify_job(_job(conclusion="cancelled", steps=[],
                              runner="")) == "starved",
           "a job with no steps and no runner must be classified starved")
    expect(classify_job(_job(conclusion="failure", steps=[],
                              runner="")) == "starved",
           "a failed job with no steps and no runner must be starved")

    # --- The genuine-failure signature ---------------------------------
    # The job ran and failed: the failure belongs to the tree.
    expect(classify_job(_job(conclusion="failure",
                              steps=[{"name": "Build"}],
                              runner="GitHub Actions 1000004724"))
           == "genuine",
           "a job that ran a step and failed must be genuine")
    expect(classify_job(_job(conclusion="failure",
                              steps=[{"name": "Build"},
                                     {"name": "Test"}],
                              runner="GitHub Actions 1000004724"))
           == "genuine",
           "a job that ran several steps and failed must be genuine")

    # --- The success and the skip --------------------------------------
    expect(classify_job(_job(conclusion="success",
                              steps=[{"name": "Build"}],
                              runner="GitHub Actions 1")) == "fine",
           "a successful job must be fine")
    expect(classify_job(_job(conclusion="skipped", steps=[],
                              runner="")) == "fine",
           "a skipped job must be fine")
    expect(classify_job(_job(conclusion=None, steps=[], runner=""))
           == "fine",
           "a job with no conclusion must be fine")

    # --- The cancelled-but-scheduled job -------------------------------
    # A runner WAS assigned, so the job was cancelled after being
    # scheduled -- a superseded run, not a starved queue.  Re-running it
    # would resurrect a run the repository has moved past.
    expect(classify_job(_job(conclusion="cancelled", steps=[],
                              runner="GitHub Actions 1000004724")) == "fine",
           "a cancelled job that had a runner must not be retried")

    # --- The concurrency cancel, measured on 2026-11-19 -----------------
    # This is the *dominant* shape in the repository: 124 of the 149
    # cancelled jobs over the last 60 cancelled runs.  They ran, so
    # they are the code's (or the superseding push's), not the
    # platform's.
    expect(classify_job(_job(conclusion="cancelled",
                              steps=[{"name": name} for name in
                                     ("Set up job", "Checkout", "Configure",
                                      "Build", "Test (debug)")],
                              runner="GitHub Actions 1000007008")) == "genuine",
           "a cancelled job that ran steps must be genuine, not starved")

    # --- The unknown shape ---------------------------------------------
    # An API response that omits `steps` is not evidence of starvation.
    unknown = _job(conclusion="failure", steps=[], runner="")
    del unknown["steps"]
    expect(classify_job(unknown) == "fine",
           "a job with no steps field must not be retried")

    # --- The selection --------------------------------------------------
    jobs = [
        _job(name="macos-clang / arm64", conclusion="cancelled", steps=[],
             runner=""),
        _job(name="linux-gcc / x86_64", conclusion="success",
             steps=[{"name": "Build"}], runner="GitHub Actions 1"),
        _job(name="windows / msvc", conclusion="failure",
             steps=[{"name": "Build"}], runner="GitHub Actions 2"),
    ]
    selected = select_starved(jobs)
    expect(len(selected) == 1 and selected[0]["name"] == "macos-clang / arm64",
           "only the starved job may be selected for a retry")

    # --- The per-job retry budget ---------------------------------------
    # The bug the run-level `run_attempt` guard caused: run 44306330102
    # had the macOS leg starved in attempt 1, was re-run, and the SAME
    # leg starved again in attempt 2.  The old guard saw
    # `run_attempt == 2` and refused; the per-job counter sees that
    # `macos-clang / arm64` has spent one retry, which is the truth.
    starved = _job(name="macos-clang / arm64", conclusion="cancelled")
    healthy = _job(name="windows / msvc", conclusion="success",
                   steps=[{"name": "Build"}], runner="GitHub Actions 2")
    spent = retries_spent([[starved, healthy], [starved, healthy]])
    expect(spent.get("macos-clang / arm64") == 1,
           "a name present in two attempts has spent exactly one retry")
    expect(spent.get("windows / msvc") == 1,
           "every name is re-run together by a job-level re-run")
    expect(not over_retry_budget("macos-clang / arm64", spent, 1),
           "after one retry a starved job is still eligible for its "
           "second -- the run-level guard refused this and left the "
           "merge gate red on 2026-11-18")
    expect(over_retry_budget("macos-clang / arm64", spent, -1),
           "with max_retries=-1 nothing is eligible, not even a fresh job")

    # A fresh job that starved for the first time has spent nothing.
    fresh = retries_spent([[starved, healthy]])
    expect(all(v == 0 for v in fresh.values()),
           "on attempt 1 no retry has been spent")

    # And a job that starved twice with a budget of 1 is exhausted.
    twice = retries_spent([[starved], [starved], [starved]])
    expect(over_retry_budget("macos-clang / arm64", twice, 1),
           "starved in three attempts means two retries have failed; "
           "with a budget of 1 the guard must stop")
    # A fresh starved job (one attempt only) is never over budget.
    singles = retries_spent([[starved]])
    expect(not over_retry_budget("macos-clang / arm64", singles, 0),
           "a job that has never been retried is within a budget of 0")

    # --- The HTTP retry policy -------------------------------------------
    expect(429 in RETRYABLE_HTTP and 502 in RETRYABLE_HTTP,
           "429 and 502 are platform wobbles and must be retried")
    expect(403 not in RETRYABLE_HTTP,
           "403 is the run-still-running race; retrying the same call "
           "cannot help and must not be treated as transient")

    # --- Waiting for the run to settle -----------------------------------
    # A run that reports `in_progress` must keep the script waiting: the
    # re-run endpoint answers 403 until it is complete.
    sleeps = []

    def fake_sleep(seconds):
        sleeps.append(seconds)

    states = iter([_run(status="in_progress"), _run(status="in_progress"),
                   _run(status="completed")])
    original = globals()["fetch_run"]
    globals()["fetch_run"] = lambda *a, **kw: next(states)
    try:
        settled = wait_until_completed(
            "o/r", 1, "t", polls=3, interval=7, sleep=fake_sleep,
            log=lambda m: None)
    finally:
        globals()["fetch_run"] = original
    expect(settled["status"] == "completed",
           "wait_until_completed must return once the run is completed")
    expect(len(sleeps) == 2,
           "wait_until_completed must poll once per outstanding state")

    # A run that never settles must be reported, not re-run: the 403 the
    # review found on 2026-11-19 came from firing at an in_progress run.
    states = iter([_run(status="in_progress")] * 4)
    globals()["fetch_run"] = lambda *a, **kw: next(states)
    try:
        stuck = wait_until_completed(
            "o/r", 1, "t", polls=2, interval=7, sleep=lambda s: None,
            log=lambda m: None)
    finally:
        globals()["fetch_run"] = original
    expect(stuck["status"] != "completed",
           "a run that never settles must be reported as unfinished")

    # --- The classification of a run that is not completed ---------------
    # `process` must not try to re-run anything from a run that has not
    # settled.
    output = []
    states = iter([_run(status="in_progress")] * 4)
    globals()["fetch_run"] = lambda *a, **kw: next(states)
    called = []
    original_rerun = globals()["rerun_job"]
    globals()["rerun_job"] = lambda *a, **kw: called.append(a)
    try:
        code = process("o/r", 1, "t", dry_run=False, max_retries=1,
                       wait_polls=2, wait_interval=7,
                       sleep=lambda s: None, log=output.append)
    finally:
        globals()["fetch_run"] = original
        globals()["rerun_job"] = original_rerun
    expect(code == 0 and not called,
           "an unfinished run must be left alone, and doing so must not "
           "turn ci-retry red")
    expect(any("still" in line for line in output),
           "an unfinished run must say so in the log")

    if failures:
        print("FAIL -- the self-test caught a misclassification:")
        for f in failures:
            print("  -", f)
        return 1
    print("PASS -- the self-test confirms every classification.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", help="owner/name of the repository")
    parser.add_argument("--run-id", type=int, help="the workflow run to inspect")
    parser.add_argument("--dry-run", action="store_true",
                        help="report what would be retried, without retrying")
    parser.add_argument("--max-retries", type=int, default=1,
                        help="how many times a single job name may be "
                             "re-run before it is considered a sustained "
                             "outage (default: 1)")
    parser.add_argument("--wait-polls", type=int, default=20,
                        help="how many times to poll while the run is not "
                             "yet completed (default: 20)")
    parser.add_argument("--wait-interval", type=float, default=15.0,
                        help="seconds between those polls (default: 15)")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    if not args.repo or not args.run_id:
        parser.error("--repo and --run-id are required")
    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if not token:
        print("ERROR: GITHUB_TOKEN (or GH_TOKEN) is not set")
        return 1

    try:
        return process(args.repo, args.run_id, token, args.dry_run,
                       args.max_retries, args.wait_polls,
                       args.wait_interval)
    except Fatal as error:
        print("ERROR: %s" % error)
        return 1


if __name__ == "__main__":
    sys.exit(main())
