# Soak Runbook — the v1.1 seven-day evidence window

> **What this document is.** The v1.1 release gate requires **seven
> consecutive days** of green end-to-end acceptance runs (Case D and
> Case E) on **Windows and Linux x86_64** before the release tag is
> cut (v1.1 plan §2 Track C, item C5). This runbook is the operating
> procedure for producing that evidence: what runs, where it writes,
> what to do when a day goes wrong, and — just as important — what the
> evidence does **not** say.
>
> The collector is `tools/run_soak.py`. The summary is read with
> `python tools/run_soak.py --summary`.

## 1. The shape of the window

| | |
|---|---|
| Requirement | 7 consecutive calendar days, both platforms, cases `d` **and** `e` green |
| What counts as a green day | Every required case has at least one `PASS` record for that platform on that calendar day, **and no required case has a `FAIL` record that day** |
| What breaks the streak | A `FAIL`, an `INCONCLUSIVE`, **or a calendar day with no evidence at all** |
| Where the evidence lives | `build/soak/<platform>-<YYYY-MM-DD>.jsonl` |
| Where the summary comes from | `python tools/run_soak.py --summary` |

Both platforms are needed. A green Windows streak and a green Linux
streak are tracked separately and **neither substitutes for the
other**; the summary prints them per platform for that reason.

The streak counts *calendar*-consecutive days. Five green days spread
across three weeks is a streak of one, not five — a gap day is a gap
day, and the tool reports those gap dates explicitly so the short
streak can be explained without diffing dates by hand.

### 1.1 The intra-day rule

A day can hold more than one record for the same case: the local
scheduled task and the CI job both write the same platform/day, and a
day may be re-run after a fix. The collector keeps **every** record,
and a day is green only if all of the following hold:

- every required case has at least one `PASS` record that day;
- **no** required case has a `FAIL` record that day.

The second bullet is the one that matters for honesty. A `FAIL` that a
later `PASS` on the same day follows is **not** erased by that `PASS`:
the failure is real evidence that something was broken, and hiding it
behind the retry would let a broken build pass the gate. Such a day is
reported as *not green*, and the failure is listed in `failures` and
in `not_green_days`.

An `INCONCLUSIVE` record is the opposite case: it means no evidence was
produced, so it does not veto a day that later produced a `PASS`.
Running twice is harmless.

Every failing record is reported, not just the first one. A day that
failed twice is a day that failed twice.

### 1.2 The tree the day was measured on

A green day is a statement about a *program*, and the program is the
tree the collector ran against. Each record therefore carries the tree
it was measured on:

| Field | Meaning |
|---|---|
| `git_ref` | the branch the tree was on (`release/1.1` for the v1.1 window) |
| `git_commit` | the commit the tree was at |
| `git_dirty` | whether the working tree carried uncommitted changes |

A day is green only when **every** record of that day was measured on
the candidate branch *and* on a clean tree:

* a day measured on another branch is evidence about a different
  program, so it is listed under "days measured on another tree";
* a day measured on a **dirty** tree is evidence about whatever the
  working directory happened to contain, not about the tagged commit —
  the commit identifies the tree, but a modified working tree is not
  that tree. Such a day is listed under "days measured on an unclean
  tree";
* a record that does not say whether the tree was clean (one written
  before the collector tracked the flag) is read as *not known to be
  clean*. The gate cannot certify what it cannot see.

The practical consequence for the release machine: **the checkout has
to be clean when the task fires.** `git status --porcelain` must print
nothing. The build tree under `build/` is ignored, so a built tree is
not a dirty tree.

## 2. Prerequisites

Per platform, the collector's precondition check runs before the
acceptance runner. When a precondition is missing the day is recorded
as `INCONCLUSIVE` (never `PASS`) and the reason is written into the
evidence file. To get real green days, all of these must hold:

| Requirement | Windows | Linux x86_64 |
|---|---|---|
| Build artefacts | `demo-p2p.exe` + `test_dtls_inproc.exe` under the build tree | `demo-p2p` + `test_dtls_inproc` (no `.exe`) |
| Playwright + Chromium | `pip install playwright` then `playwright install chromium` | same |
| `websockets` package | `pip install websockets` | same |
| Chrome | system Chrome, or Playwright's bundled Chromium | system Chrome, or Playwright's bundled Chromium |

The build artefacts are located under `$NIMRTC_BUILD_DIR` (default
`build/`), in either the MSVC layout (`<build>/examples/Debug/`) or
the single-config layout (`<build>/examples/`). A machine that builds
out of tree must export `NIMRTC_BUILD_DIR` before the scheduled task
runs, or the day is recorded as `INCONCLUSIVE`.

Build the project before the first day of the window. The window is
seven days of *evidence*, and a day spent rebuilding is a day the
window has not reached.

## 3. Daily invocation

The collector runs **one day at a time** and appends to that day's
evidence file. Running it twice in a day is harmless — the second run
appends a second record for the same day and the summary reads the
day as green only if every required case has a `PASS`.

```bash
# Windows
python tools\run_soak.py --platform windows --case d,e

# Linux x86_64
python3 tools/run_soak.py --platform linux-x86_64 --case d,e
```

Exit codes: `0` all cases passed, `1` at least one case failed, `2`
the tool could not run at all (runner missing, bad arguments).

### 3.1 The escape hatch this tool refuses

The acceptance runner honours `NIMRTC_SKIP_CASE_D=1` by writing a
**synthetic PASS** for Case D without running anything. It is a
historical CI workaround for branches that predate the DTLS handshake
reassembly fix.

A synthetic PASS is indistinguishable from a real one in the runner's
exit code, so `run_soak.py` **refuses to run** while that variable is
set, and says why. Recording one as a soak day would put a fabricated
green into a release gate.

## 4. Scheduling

The window only exists if it runs without someone remembering to run
it. Both platforms are expected to schedule the collector daily.

### 4.1 Windows — Task Scheduler

Run once a day, at a time when the machine is on and the build tree
is intact. A scheduled task that runs while the machine is asleep
produces no record, and a missing day breaks the streak.

The task is registered by a script, so that registering it is a
reviewable, repeatable act rather than a copy-paste:

```powershell
# Register the daily task (03:07 local by default).
.\scripts\register-soak-task.ps1

# ... or with a different time.
.\scripts\register-soak-task.ps1 -Time 03:17

# ... or remove it again.
.\scripts\register-soak-task.ps1 -Remove
```

The registration wires `scripts/run-soak-daily.ps1` to the task. That
wrapper runs the collector, tees its output into
`build/soak/soak-task.log`, and returns the collector's exit code
(0 = every case passed, 1 = a case failed, 2 = the collector could
not run). It also picks the interpreter: the first candidate that can
both execute code and import `playwright`, because Case D drives a
real Chrome peer through Playwright and an interpreter without it
cannot produce evidence at all.

`StartWhenAvailable` is on, so a trigger missed while the machine was
asleep fires as soon as the machine is back — a gap day breaks the
streak, so a task that never runs at all is worse than one that runs
late.

The equivalent hand-rolled registration, for reference:

```powershell
$action  = New-ScheduledTaskAction `
    -Execute "python" `
    -Argument "d:\MyOpen\NimRTC\tools\run_soak.py --platform windows --case d,e" `
    -WorkingDirectory "d:\MyOpen\NimRTC"
$trigger = New-ScheduledTaskTrigger -Daily -At 3am
Register-ScheduledTask `
    -TaskName "NimRTC soak" `
    -Action $action -Trigger $trigger `
    -Description "Daily NimRTC v1.1 soak evidence collection (v1.1 plan C5)"
```

Read the result with:

```powershell
Get-ScheduledTaskInfo -TaskName "NimRTC soak" | Select-Object LastRunTime, LastTaskResult
```

`LastTaskResult` of `0` means the collector ran and every case passed.
A non-zero value means a FAIL, or that the precondition check tripped.

### 4.2 Linux — cron

```cron
# /etc/cron.d/nimrtc-soak
# Run at 03:17 daily. The odd minute is deliberate: a job scheduled
# at :00 competes with every other :00 job on the machine.
17 3 * * * nimrtc cd /opt/nimrtc && /usr/bin/python3 tools/run_soak.py --platform linux-x86_64 --case d,e >> build/soak/cron.log 2>&1
```

The collector must be able to write to `build/soak/`, and the cron
user needs a build tree it can read.

### 4.3 GitHub Actions — the scheduled CI collector

A cron entry needs a machine that is switched on at the right minute.
The repository therefore also ships a scheduled workflow,
`.github/workflows/soak.yml`, that runs the same collector on the
GitHub-hosted runners:

| | |
|---|---|
| Schedule | `cron: '23 3 * * *'` (daily; `workflow_dispatch` runs one platform on demand) |
| Jobs | `linux-x86_64` (ubuntu-22.04) and `windows` (windows-2022) |
| What it runs | `python tools/run_soak.py --platform <platform> --case d,e` on a fresh Debug build |
| Which tree it measures | `SOAK_REF` — `release/1.1` by default, overridable with the `ref` dispatch input |
| Where the record lands | `build/soak/<platform>-<YYYY-MM-DD>.jsonl`, uploaded as the artifact `soak-<platform>-<run id>` (90-day retention) |
| Where it accumulates | in the artifacts — one artifact per run, which is where the record of the window lives |
| What the Step Summary shows | the whole window: the workflow pulls the earlier days' artifacts back into `build/soak/` before it summarises |

**The artifact is the record.** Every run of the workflow uploads its
own artifact, so a day that is measured twice produces two artifacts
and **both** sets of records are kept. That matters: the collector
never lets a failing run be hidden by a later pass on the same day,
and a mechanism that kept only the last run's file would hide exactly
that.

The records used to accumulate on a per-platform branch
(`soak/evidence-<platform>`) instead, one commit per day. That was a
permanent ref that grew without bound and had to be kept alive, and it
was lossy — the workflow *copied* each day's file onto the branch, so
a second run of the same day replaced the first run's records. It was
also what broke the workflow: the step that pushed the branch replaced
the working tree with the branch's tree, which deleted
`tools/run_soak.py` from the checkout, so the summary step that ran
after it could not find its own tool. On Linux the failure was
invisible on top of that, because the summary was piped into `tee`
without `pipefail`, so the step reported success with an empty Step
Summary. Those branches are gone; the artifacts replace them.

### 4.3.1 Which tree the window measures

The window must measure **the tree that gets tagged**, which for
`v1.1.0` is `release/1.1` — *not* `main`. `main` already carries the
v1.2.0 audio body, so evidence collected from `main` would be evidence
about a different program than the release candidate. The workflow
therefore checks out `SOAK_REF`, which defaults to `release/1.1`:

```yaml
env:
  SOAK_REF: ${{ inputs.ref || 'release/1.1' }}
```

A scheduled run has no `inputs`, so it takes the same default; a
manual run can point it elsewhere with `workflow_dispatch` → `ref`.

**The Windows half of the window has the same requirement, and it is
not automatic.** The local Scheduled Task (§4.1) runs the collector
against *the working tree of the checkout it was registered against* —
it does not check out a ref. If that checkout is on `main` (because
the v1.2 body is being worked on), the day's records carry
`git_ref = "main"` and the summary reports the day as *measured on
another tree*: it does not count towards the window, and it is listed
explicitly rather than silently dropped. Nothing is lying and nothing
has to be deleted by hand.

Two consequences follow:

* The **pinned source** for both halves of the window is the GitHub
  workflow, whose two jobs (`linux-x86_64` and `windows`) both check
  out `SOAK_REF` and therefore always measure the candidate tree.
* The local Scheduled Task is the **independent second source**. Its
  days count when it happened to measure the candidate branch, and do
  not count otherwise.

To make the local task a full contributor, the release machine's
checkout has to be on the candidate branch when it fires. Check it
before letting a day be recorded:

```powershell
git -C d:\MyOpen\NimRTC rev-parse --abbrev-ref HEAD
```

Nothing else has to be done when the branch is not the candidate one:
the day is already excluded from the window by the `git_ref` rule, and
the CI leg of the same platform covers the day regardless.

Both jobs run with `continue-on-error: true`: a red acceptance run is
**evidence** (the collector records `FAIL`, which breaks the streak),
not a build break, so the window's verdict is read from the evidence
file and the Step Summary rather than from a red check on `main`.

Reading the CI-produced evidence on the release machine:

```powershell
# Pull the window's CI half back into build/soak/, then read the window.
python tools/fetch_soak_evidence.py --days 21
python tools/run_soak.py --summary --require
```

`tools/fetch_soak_evidence.py` reads the workflow's artifacts through
the GitHub API and writes their records into `build/soak/`, under the
same file names the collector uses (`<platform>-<day>.jsonl`). It runs
on Windows and on Linux, so the release machine needs nothing else —
the branch recipe this replaces was bash-only and could not run on the
Windows host that owns the window.

Three properties of the fetch are worth knowing:

* It **appends**, never overwrites: the Windows Scheduled Task and the
  CI job both produce a record for the same platform and day, and the
  summary needs both (it reads a day as green only when every required
  case has a PASS record and no case has a FAIL).
* It is **idempotent**: a record that is already in the file is not
  written again, so running it every night leaves the files
  byte-identical instead of growing them.
* It **fails** when an artifact cannot be read. A window read from an
  incomplete set of artifacts is not the window, and a short read must
  not be mistaken for a short window.

The token comes from the same places `tools/gh_api.py` looks: the git
credential store first, then `GH_TOKEN` / `GITHUB_TOKEN`.

Useful flags: `--platform windows` fetches one platform's half only,
`--since 2026-11-01` sets the earliest run date by hand, and
`--dry-run` lists what would be fetched without writing anything.

The summary defaults to `--expect-ref release/1.1`, so a day measured
on any other tree is excluded from the window and listed under "days
measured on another tree". Pass `--expect-ref ""` to switch the check
off (for example when summarising evidence for a different release).

Run the fetch **before** every `--summary --require`: the summary only
sees what is in `build/soak/`.

**The artifacts expire.** They are kept for 90 days, which is far more
than a seven-day window needs — but the records of the window should be
filed with the release evidence when the window closes (§7), because
after that the artifacts are the only place they live.

The workflow is the only source of the Linux half of the window unless
a real Linux machine is available; a green Windows streak and a green
Linux streak are tracked separately and neither substitutes for the
other.

## 5. Reading the window

```bash
python tools/run_soak.py --summary
```

```
soak summary (cases: d, e)

linux-x86_64:
    days with evidence    : 4
    consecutive green days: 1
    best streak so far    : 3
    days without evidence : 1 (these break the streak)
        2026-11-04

windows:
    days with evidence    : 7
    consecutive green days: 7
    best streak so far    : 7
```

Note the Linux numbers above: three green days, then a gap on
2026-11-04, then one more green day. The **best** streak was three,
but the **current** streak is one — the gap ended it. A window is
satisfied by the current streak, so this Linux half is not there yet.

`--json` emits the same content machine-readably, for a gate script
that wants to assert on it rather than read it.

### 5.1 What each line tells you

- **days with evidence** — how many calendar days carry a record for
  this platform.
- **consecutive green days** — the current streak, ending at the most
  recent recorded day. This is the number the gate cares about.
- **best streak so far** — the longest run anywhere in the history.
  A window is satisfied by the *current* streak, not the best one: a
  window that was green in week one and red in week two has not
  proven seven consecutive green days.
- **days without evidence** — the calendar days between the first and
  last record that carry no record at all. Each one breaks the streak.
- **inconclusive days** — days where the collector ran but the
  preconditions were not met. They are never green, and they break
  the streak.
- **failures** — every failing record, with its day, case and reason. A
  failure is never hidden by a later `PASS` on the same day (§1.1).
- **days with evidence but not green** — days that carry evidence but
  fail the green test, with the cases that lacked a `PASS` or carried a
  `FAIL`. This is where a day that a retry "fixed" still shows up.
- **days measured on another tree** / **days measured on an unclean
  tree** — the days whose records do not describe the candidate
  commit. They are not green, and they are listed rather than dropped,
  so a short streak can be explained without guessing (§1.2).
- A tracked platform with **no evidence at all** is listed with a zero
  streak rather than being omitted. A platform missing from the report
  reads as "not tracked", which is exactly the misunderstanding the
  release gate has to prevent.

## 6. When a day goes wrong

| Symptom | Meaning | What to do |
|---|---|---|
| `INCONCLUSIVE`, "required build artefacts are missing" | The build tree moved or was cleaned | Rebuild, or export `NIMRTC_BUILD_DIR` for the scheduled task |
| `INCONCLUSIVE`, "the `playwright` package is not importable" | The Python environment for the scheduled task lacks Playwright | Install Playwright into **that** interpreter, not the one in your interactive shell |
| `FAIL`, "the acceptance runner exited N" | A real acceptance failure | Read `build/e2e/case_d_chrome.log` / `case_e_harness.log` — this is a genuine regression, not a tooling problem |
| `FAIL`, "signaling_server did not open :<port>" | The signaling server was still starting when the harness stopped waiting | Check `build/e2e/case_d_signal_server.log` — an empty log means the server never got far enough to print anything (a cold start), a log with an error means it crashed. The runner now waits 30 s and prints the log tail, so this is distinguishable from a real failure |
| `FAIL`, "did not complete (timeout or launch error)" | The runner hung past 30 minutes or could not start | Check for a leftover Chrome or `demo-p2p` holding port 8765 or 9000 |
| No record for a day at all | The scheduled task did not run | Check the task history; a machine that was asleep produces a gap, and a gap breaks the streak |

**A day that fails does not "cost" a day that can be recovered.** The
window is seven *consecutive* green days; a failure resets the streak
and the count restarts from the next green day. That is the intended
behaviour: the gate is asking for a week of unbroken green, and
patching over a red day would defeat the point of having one.

**An INCONCLUSIVE day is not a failure.** It means no evidence was
produced — the machine was not ready — and it is reported separately
so a tooling problem is never mistaken for a product regression. It
still breaks the streak, because a day without evidence cannot be
counted towards a window that is about evidence.

## 7. Closing the window

The window is satisfied when `python tools/run_soak.py --summary`
shows **7 consecutive green days on both platforms**. Record the
summary output alongside the release evidence, then update the C5 row
in `docs/plan/v1.1-plan.md`.

File the **records** as well as the summary output. The CI half of the
window lives in the workflow artifacts, and the artifacts expire after
90 days; the copy that is pulled into `build/soak/` when the window
closes is the one that has to be kept.

Two things the window does **not** establish:

- It does not certify the build that gets tagged. The soak runs
  against a working tree; the tag is cut from a commit. Make sure the
  tree is clean and at the intended commit before starting the
  window, and that it stays there.
- It does not replace the other release gates. C3 (freeze checklist),
  C4 (tag signature) and the Track A/B/D gates are independent.

## 8. Verifying the collector itself

The collector's honesty rules are the point of the tool, so they are
pinned by a self-test:

```bash
python tools/run_soak.py --self-test
```

It asserts, among other things, that gapped green days do not form a
long streak, that a failing or inconclusive day breaks the streak,
that a day missing one required case is not green, that six green
days report a streak of six and not seven, and that the two platforms
are tracked independently.

It also pins the rules that keep the tool from overstating the
evidence: a day whose `FAIL` is followed by a `PASS` is **not** green
and the failure is still reported; an `INCONCLUSIVE` followed by a
`PASS` **is** green; every failing record is reported, not just the
first; a tracked platform with no evidence at all appears in the
summary with a zero streak and fails the gate; and an exactly
duplicated record is loaded once.

Run it after changing anything in `run_soak.py`.
