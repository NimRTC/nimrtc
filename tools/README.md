# tools/

Development and release utility scripts (not part of the NimRTC library itself).

## Directory structure

```
tools/
├── *.py            # Utility scripts (lint, release, codegen, CI helpers)
├── *.sh / *.cmd    # Thin wrappers for platform-specific commands
├── cmake/          # CMake helper scripts used at configure time
├── scripts/        # Shell / Python utilities (lint, release, codegen)
└── ci/             # CI-specific scripts (packaging, signing)
```

## Conventions

- All scripts must have a shebang (`#!/bin/bash` or `#!/usr/bin/env python3`)
- Scripts that modify source must be idempotent (safe to re-run)
- No credentials or secrets — use environment variables only
- Document usage in a `--help` block or a top-level comment

## Build scripts

| Script | Platform | Purpose |
|--------|----------|---------|
| `scripts/build.bat` | Windows | Interactive build: configure + build + test (MSVC preset) |
| `scripts/build.sh` | Linux/macOS | Interactive build: configure + build + test (GCC/Clang preset) |

Both scripts:
- Auto-detect the installed compiler
- Choose Debug/Release/asan preset interactively (or via flags)
- Run tests after build (ask confirmation, skippable)
- Support `--rebuild`, `--configure`, `--build`, `--test` flags

```
.\scripts\build.bat --rebuild --release   # Windows: clean + Release
bash scripts/build.sh --rebuild --preset=release  # Linux/macOS
```

## Documentation

| Script | Purpose |
|--------|---------|
| `check_md_links.py` | Verify relative Markdown links and `#anchor` targets in every non-vendored `.md` file. Wired into CI as the `docs / markdown links` job. |

```
python tools/check_md_links.py            # whole repo
python tools/check_md_links.py README.md  # specific files
```

Exit code is non-zero when a link points at a missing file or an anchor
that no heading produces.

## The plugin-catalogue contract

| Script | Purpose |
|--------|---------|
| `check_doc_contract.py` | Check the §4 catalogue in `docs/plugin_author_guide.md` against the code: every `EngineConfig` knob it names must exist, every registration macro it names must exist, and every `NIMRTC_REGISTER_*` macro in the registry must be listed. Wired into CI as the `docs / markdown links` job. |

```
python tools/check_doc_contract.py
```

The catalogue is a promise about the code, so it has to be checked in both
directions. A knob the guide names but the engine does not have sends the
author to a runtime failure; a macro the guide forgot to list sends them
looking for a seam that exists but is undocumented. The check is a text
check rather than a C++ reflection check, which keeps it cheap enough to
run on every push.

## The dead-module guard

| Script | Purpose |
|--------|---------|
| `check_dead_modules.py` | Architecture-remediation D2: "either wire it in, or delete it." A module under `src/modules/` with a complete implementation and passing tests, but no path from the composition root, is the worst state — it manufactures the illusion that a feature exists. The check reads the CMake dependency graph and the plugin registrar table. Wired into CI and into CTest as `test_dead_modules`. |

```
python tools/check_dead_modules.py --verbose
python tools/check_dead_modules.py --self-test
```

Three failure shapes are reported separately, because the remedy differs:

| Verdict | Meaning | Remedy |
|---------|---------|--------|
| `[dead]` | consumed only by its own tests — a complete implementation the product never calls | wire it in, or delete it |
| `[unreach]` | linked only by a target that never ships (a benchmark) — the link line makes it look alive | wire it in, or delete it |
| `[orphan]` | nobody links it and nothing references its registrar | wire it in, or delete it |

A module that registers a plugin factory counts as live when the registrar
table references it: the registration *is* the consumption. The
`--self-test` builds a synthetic module tree and asserts that the checker
still distinguishes each shape, so the guard cannot silently degrade into
a no-op.

## The engine-layering guard

| Script | Purpose |
|--------|---------|
| `check_engine_layering.py` | ADR-025 Layout Invariant 4: the engine reaches a concrete module only through its `plugins::I*` seam, with a named set of exemptions. The check scans `src/engine/` for concrete-module includes and fails when a module outside the ADR-025 Decision 1 table appears. Wired into CI as the `docs / markdown links` job. |

```
python tools/check_engine_layering.py --verbose
python tools/check_engine_layering.py --self-test
```

The exemption list lives in the script, one commented entry per module,
so widening it is a visible act rather than an accident — an exemption
that grows silently is how the invariant stopped meaning anything in the
first place.

## The C-clean header guard

| Script | Purpose |
|--------|---------|
| `check_c_clean_headers.py` | ADR-026: a `.h` under `src/**/include/` is a promise that a C host can include it. The check compiles every such header as C11, in isolation, with a real C compiler. Wired into CI as the `docs / markdown links` job. |

```
python tools/check_c_clean_headers.py --verbose
python tools/check_c_clean_headers.py --self-test
python tools/check_c_clean_headers.py --require-compiler
```

Isolation is the point: a header that only compiles because some *other*
header was included first is exactly the header that breaks a C consumer
who includes it first. Each header is probed by its public name — the
path below its module's `include/` root — so the check tests the include
contract rather than the file system.

The scope is the extension: `.h` is C-consumable, `.hpp` is C++-only. A
header that was always C++-only opts out by being renamed; a header that
was always meant to be C cannot drift.

## CI sanitizer gate

`linux-sanitizers` in `.github/workflows/ci.yml` runs the test suite under
AddressSanitizer / UndefinedBehaviorSanitizer / ThreadSanitizer. The
vendored stacks trip the sanitizers on their own, so the job would either
be red all the time or have to ignore everything. `sanitizer_gate.py`
classifies each report by the ownership of its root frame and fails only
on the ones that are ours.

| Verdict | Meaning | Fails the job |
|---------|---------|----------------|
| `owned` | the root frame is in `src/` or `tests/` | yes |
| `vendor` | the root frame is in `src/third_party/` | no |
| `shared` | upstream root, NimRTC is the other party to the fault | only with `--fail-on-shared` |
| `unknown` | no frame could be attributed | no |

A test failure that no upstream report explains is a plain test failure
and fails the job as well.

```
python tools/sanitizer_gate.py build/sanitizer-ctest.log
python tools/sanitizer_gate.py --ctest-status build/ctest-exit.txt build/Testing/Temporary/LastTest.log
python tools/sanitizer_gate.py --self-test
```

The self-test runs in the `docs / markdown links` job on every push.

## The merge-gate guard

The repository's merge gate is a GitHub Ruleset, mirrored in
`.github/rulesets/protect-main.json` so that a change to the gate is a
pull request rather than a click in the web UI.

| Script | Purpose |
|--------|---------|
| `check_ci_ruleset.py` | Compares the ruleset mirror against the CI workflows and fails when the two disagree. Wired into CI as the `docs / markdown links` job and into CTest as `test_ci_ruleset`. |
| `apply_ci_ruleset.py` | Writes the mirror to the GitHub repository (`--apply`), or reports the difference (`--check`). Needs a token with write access. |

```
python tools/check_ci_ruleset.py --verbose
python tools/check_ci_ruleset.py --self-test
python tools/apply_ci_ruleset.py --check
python tools/apply_ci_ruleset.py --apply
```

The two sets have to be **equal**, not merely one a subset of the other,
because both directions fail badly:

| Failure | Symptom |
|---------|---------|
| the gate requires a check no workflow produces | every merge is blocked, forever, with nothing to fix from the PR page |
| the matrix produces a check the gate does not require | the check costs runner time and catches nothing |

The trap the guard was written for is `windows / fast`: it comes from
`ci-fast.yml`, which deliberately does *not* run on `main`
(`branches-ignore: [main, 'release/**']`), so requiring it would
deadlock the repository.

A check counts as "produced" only when its workflow runs both on a push
to `main` **and** on a pull request into `main`.  The second half is
the one that is easy to miss: `pull_request: branches: [main]` filters
on the PR's *base* branch, so a workflow that omits it never runs for
the merge it is supposed to be gating.

## The saturated-link benchmark (plan E6)

| Script | Purpose |
|--------|---------|
| `benchmark_saturation.py` | Drives `tests/bench/bench_saturation.cpp` — the shipped bandwidth estimator, the shipped rate controller with its resolution ladder, and the shipped pacer, over a **real UDP socket pair** whose receiving side enforces a token-bucket capacity and **drops the datagrams that overflow it**. The congestion is therefore real loss of real packets, not a simulated loss percentage. Asserts nine properties of the run (the saturation was real, the stream survived, the ladder reacted, the estimator never raised its target on a congested report, the delivered rate stayed within capacity, the offered load tracked the capacity, the ladder recovered, the throughput recovered, the ladder did not flap) and writes the artifact. Wired into CI as the `Benchmark saturated link (plan E6)` step in both the Windows and the Linux legs. |

```
python tools/benchmark_saturation.py --build-dir build/e6 \\
    --out docs/benchmarks/saturation_windows_x86_64.json
python tools/benchmark_saturation.py --build-dir build/e6 --dry-run
```

The binary is deliberately **not** a ctest case: every number in it is
machine-dependent (the loopback's real RTT, the host's real scheduling
jitter), so a threshold assertion would be a flaky test rather than a
measurement. The properties that must hold regardless of the machine
are asserted by the wrapper instead, and they are stated as
*properties* rather than as *thresholds against a reference number*.

Two scenarios are measured, and they differ **only** in the resolution
floor:

| Scenario | Floor | What it exercises |
|----------|-------|-------------------|
| `floor` | 720p | The product's legibility floor. The lowest allowed rung is twice the link's capacity, so the picture cannot be made small enough to fit — the ladder must hold the floor and let the bitrate be throttled instead. |
| `unfloored` | none | The ladder may descend to a rung that *does* fit, so the offered load converges onto the link's capacity. |

The estimator is a knob (`--bwe aimd` / `--bwe goog-cc`) rather than a
scenario, and the artifact records which one produced the numbers.

The `estimate_did_not_climb_while_congested` invariant is the
regression guard for the AIMD defect this benchmark uncovered: the
AIMD front-end used to resume its additive increase once the 500 ms
cooldown expired even while the link was *still* lossy, so the estimate
walked back up to (and past) the rate that had just been throttled. The
Goog-CC front-end already carried that guard; the AIMD one did not.

The guard is decided on the estimator's own **per-report reason**
(`aimd_increase` / `remb_override` on a report whose loss rate is at or
above the estimator's 2% congestion threshold), not on the before/after
estimate pair: the reported estimate is an exponentially smoothed value,
so it can drift upwards by a few bits after a decrease without the
estimator having raised anything. The artifact's `reports` array
carries the raw evidence, and a failure itemises the offending reports
rather than only reporting a count.

### The busy host

The sender is a *process on the host*, and a busy host cannot fill the
link: it delivers less than the controller asked for, so the link never
queues, nothing is dropped, and the two invariants that only mean
something under real saturation — *delivered stayed within the
capacity* and *offered load tracked the capacity* — have no answer.

That case is detected by comparing the delivered rate against the
controller's own target, and it is handled as a **skip**, not as a
failure: a measurement that did not happen is not a result, and failing
it would report a rate-control bug that is really a busy CI runner. The
wrapper re-runs the scenario up to `MAX_SCENARIO_ATTEMPTS` times, and
after that the scenario is a **failure** — a gate that cannot measure is
not a gate. The verdict's `skips` array lists every property that went
unchecked, so a green run cannot hide one.

## Release evidence (the seven-day soak)

The v1.1 release gate needs one evidence record per platform per day.
The operating procedure is `docs/plan/soak-runbook.md`.

| Script | Purpose |
|--------|---------|
| `run_soak.py` | Runs the acceptance cases and records one machine-readable record per platform per day under `build/soak/`. `--summary` prints the streak it can prove, `--require` turns that summary into a gate (exit 1 unless every platform has 7 consecutive green days), `--self-test` pins the honesty rules. |
| `fetch_soak_evidence.py` | Pulls the CI-produced records back out of the workflow artifacts into `build/soak/`. Idempotent, and fails when an artifact cannot be read. |

```
python tools/run_soak.py --summary --require      # read the window
python tools/fetch_soak_evidence.py --days 21      # fold the CI half in
python tools/fetch_soak_evidence.py --self-test
```

## Scripts planned

| Script | Phase | Purpose |
|--------|-------|---------|
| `check_prerequisites.py` | **P0 (done)** | Check CMake/compiler/Ninja/Python versions before build |
| `scripts/codegen.py` | P1 | Generate codec registration tables from SDP media types |
| `scripts/release.py` | P1 | Bump version, tag, build release artifacts |
| `scripts/lint.py` | P0 | Wrapper around clang-tidy, cmake-lint |
| `ci/package.sh` | P1 | Build distribution packages (.tar.gz, .zip) |
