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

    fine          the job succeeded, or it was skipped, or it was
                   cancelled for a reason that is not runner starvation
                   (a superseded run, a concurrency cancel).

The two signatures are disjoint and have been checked against real
runs of this repository:

    run 43938323684  macos-clang / arm64   steps=0   runner_name=""   -> starved
    run 43877514623  macos-clang / arm64   steps=19  runner_name set -> genuine
    run 43864579652  linux-sanitizers / asan-ubsan  steps=11, runner set -> genuine

A job that *ran* and failed is never retried, whatever the failure
looks like.

## The loop guard

A retry can itself be starved.  The script therefore only retries a
run's **first** attempt: if the second attempt is starved too, the
platform is having a sustained outage and more retries only burn
runner minutes.  The script reports that state and stops.

## Usage

    python tools/ci_retry_runner.py --repo owner/name --run-id 12345
    python tools/ci_retry_runner.py --repo owner/name --run-id 12345 --dry-run
    python tools/ci_retry_runner.py --self-test

The token comes from `GITHUB_TOKEN` (or `GH_TOKEN`).

Exit code 0 when the run needed no retry or the retry was requested
successfully, 1 on an API error.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.error
import urllib.request

API_ROOT = "https://api.github.com"
API_VERSION = "2022-11-28"

# The conclusions a job can end on.  `cancelled` is included because a
# starved job is reported as cancelled, not as failure.
CONCLUSIONS = ("failure", "cancelled")


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


# ---------------------------------------------------------------------------
# The GitHub API
# ---------------------------------------------------------------------------

def _request(method: str, url: str, token: str, payload=None):
    data = None
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(url, data=data, method=method)
    request.add_header("Authorization", "Bearer %s" % token)
    request.add_header("Accept", "application/vnd.github+json")
    request.add_header("X-GitHub-Api-Version", API_VERSION)
    if data is not None:
        request.add_header("Content-Type", "application/json")
    with urllib.request.urlopen(request, timeout=60) as response:
        body = response.read()
    if not body:
        return {}
    return json.loads(body.decode("utf-8"))


def fetch_run_jobs(repo: str, run_id: int, token: str) -> list:
    """Every job of a run, across pages."""
    jobs = []
    page = 1
    while True:
        url = ("%s/repos/%s/actions/runs/%d/jobs?per_page=100&page=%d"
               % (API_ROOT, repo, run_id, page))
        body = _request("GET", url, token)
        batch = body.get("jobs") or []
        jobs.extend(batch)
        if len(batch) < 100:
            break
        page += 1
    return jobs


def fetch_run(repo: str, run_id: int, token: str) -> dict:
    url = "%s/repos/%s/actions/runs/%d" % (API_ROOT, repo, run_id)
    return _request("GET", url, token)


def rerun_job(repo: str, job_id: int, token: str) -> None:
    url = "%s/repos/%s/actions/jobs/%d/rerun" % (API_ROOT, repo, job_id)
    _request("POST", url, token)


# ---------------------------------------------------------------------------
# The action
# ---------------------------------------------------------------------------

def process(repo: str, run_id: int, token: str, dry_run: bool,
            max_attempt: int) -> int:
    run = fetch_run(repo, run_id, token)
    attempt = run.get("run_attempt", 1)

    jobs = fetch_run_jobs(repo, run_id, token)

    print("run %d (%s) attempt %d -- %s"
          % (run_id, run.get("name"), attempt, run.get("conclusion")))

    for job in jobs:
        verdict = classify_job(job)
        marker = {"starved": "STARVED", "genuine": "GENUINE",
                  "fine": "ok     "}[verdict]
        print("  %-8s %-34s conclusion=%-9s steps=%s runner=%s"
              % (marker, job.get("name"), job.get("conclusion"),
                 len(job.get("steps") or []),
                 job.get("runner_name") or "<none>"))

    starved = select_starved(jobs)
    if not starved:
        print("nothing to retry: no job was starved of a runner")
        return 0

    if attempt > max_attempt:
        print("attempt %d exceeds the retry limit (%d): the platform is "
              "having a sustained outage and more retries would only burn "
              "runner minutes.  Reporting instead of retrying."
              % (attempt, max_attempt))
        return 0

    for job in starved:
        name = job.get("name")
        job_id = job.get("id")
        if dry_run:
            print("would rerun %s (job %d)" % (name, job_id))
            continue
        try:
            rerun_job(repo, job_id, token)
        except urllib.error.HTTPError as error:
            print("ERROR: rerunning %s (job %d) failed: %s %s"
                  % (name, job_id, error.code, error.reason))
            return 1
        print("reran %s (job %d) -- it had been starved of a runner"
              % (name, job_id))
    return 0


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
    parser.add_argument("--max-attempt", type=int, default=1,
                        help="the highest run attempt that may be retried "
                             "(default: 1 -- only the first attempt)")
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

    return process(args.repo, args.run_id, token, args.dry_run,
                    args.max_attempt)


if __name__ == "__main__":
    sys.exit(main())
