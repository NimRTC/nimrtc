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
| What counts as a green day | Every required case has a `PASS` record for that platform on that calendar day |
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

```powershell
# Register a daily task. Run once, from an elevated prompt.
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
| Where the record lands | `build/soak/<platform>-<YYYY-MM-DD>.jsonl`, uploaded as the artifact `soak-<platform>-<run id>` (90-day retention) |
| Where it accumulates | the per-platform branch `soak/evidence-<platform>`, one commit per day, so the two jobs never race for the same ref |

Both jobs run with `continue-on-error: true`: a red acceptance run is
**evidence** (the collector records `FAIL`, which breaks the streak),
not a build break, so the window's verdict is read from the evidence
file and the Step Summary rather than from a red check on `main`.

Reading the CI-produced evidence on the release machine:

```bash
# Fetch both evidence branches and drop their records into build/soak/
git fetch origin \
  'refs/heads/soak/evidence-linux-x86_64:refs/remotes/origin/soak/evidence-linux-x86_64' \
  'refs/heads/soak/evidence-windows:refs/remotes/origin/soak/evidence-windows'
for branch in soak/evidence-linux-x86_64 soak/evidence-windows; do
  git ls-tree -r --name-only "origin/$branch" | grep '\.jsonl$' | while read -r f; do
    mkdir -p "build/soak/$(dirname "$f")"
    git show "origin/$branch:$f" > "build/soak/$f"
  done
done
python tools/run_soak.py --summary --require
```

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
- **failures** — days where the acceptance runner exited non-zero.

## 6. When a day goes wrong

| Symptom | Meaning | What to do |
|---|---|---|
| `INCONCLUSIVE`, "required build artefacts are missing" | The build tree moved or was cleaned | Rebuild, or export `NIMRTC_BUILD_DIR` for the scheduled task |
| `INCONCLUSIVE`, "the `playwright` package is not importable" | The Python environment for the scheduled task lacks Playwright | Install Playwright into **that** interpreter, not the one in your interactive shell |
| `FAIL`, "the acceptance runner exited N" | A real acceptance failure | Read `build/e2e/case_d_chrome.log` / `case_e_harness.log` — this is a genuine regression, not a tooling problem |
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
are tracked independently. Run it after changing anything in
`run_soak.py`.
