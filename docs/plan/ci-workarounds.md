# CI Workarounds Register

> **Status: active.** This file is the single place where every CI-only
> or build-only workaround is enumerated, with the reason it exists and
> the condition for removing it.
>
> **Rule:** v1.0.0 does not ship with an *undocumented* disabled CI step
> or a silent build workaround. If a workaround is not listed here, it is
> a bug in the CI config, not an accepted trade-off.

| | |
|---|---|
| Owner | BDFL (1-person + Cursor baseline) |
| Introduced | 2026-10-21 (consolidated from scattered `CHANGELOG.md` / YAML comments) |
| Review cadence | Every release pre-flight (`RELEASING.md` §3.1) |
| Blocks v1.0.0? | **W1 and W2 do not.** **W3 does** (see table) — **W3 was closed on 2026-11-06**, so it no longer blocks anything; W4 and W5 are closed as well. |

---

## Why this file exists

The v0.14.0 cycle accumulated several build- and CI-level accommodations
that were each individually reasonable but collectively undocumented:

- two of them lived only as comments inside `.github/workflows/release.yml`,
  which nobody reads during a release;
- one was a CMake function with a 20-line explanatory comment in
  `src/third_party/CMakeLists.txt`;
- the aarch64 status was recorded in `CHANGELOG.md` prose that said
  "may be TEMP-DISABLED" without saying whether it *currently was*.

None of them were wrong. The problem is that "is the aarch64 leg
currently running?" required reading four files and reconstructing
history. This register answers that in one table.

---

## The register

| ID | Workaround | Where | Why | Impact if removed | Removal condition | Blocks v1.0.0? |
|---|---|---|---|---|---|---|
| **W1** | msquic forced onto the dummy raw datapath | `src/third_party/.gitmodules` (msquic URL → NimRTC/msquic, branch `nimrtc-patches`) + commit `a5ae3a06e` on the fork | The Windows XDP SDK (`xdp-for-windows`) is not present on the `windows-2022` runner image, so msquic's real datapath fails to compile with a missing `xdp/wincommon.h`. msquic is therefore pinned to a NimRTC fork (`https://github.com/NimRTC/msquic`), whose `nimrtc-patches` branch is upstream `microsoft/msquic` `42376ae` plus one NimRTC commit that flips the platform `CMakeLists.txt` condition to force `datapath_raw_dummy.c`. The patch is committed in the fork, not rewritten at configure time, so the submodule working tree stays vendor-clean. | Windows CI goes red at configure/compile. | Add the `xdp-for-windows` SDK to the runner image, then re-point the msquic submodule at `microsoft/msquic` and drop the fork branch — the dummy-datapath patch would no longer be needed. | No |
| **W2** | `target_compile_options(core PRIVATE /wd28020)` | `src/third_party/CMakeLists.txt` | msquic's `core` target compiles `src/core/crypto_tls.c` under MSVC `/analyze /WX /sdl`, tripping a known false-positive `C28020` (SAL value-range) on a macro-driven `TlsRead`/`TlsWrite` switch. Scoped `PRIVATE` to the vendored `core` target so it does not leak into NimRTC's own `/W4 /WX` targets. | MSVC analysis fails the msquic build. | Upstream msquic suppresses or fixes C28020; or NimRTC stops building msquic with `/analyze`. | No |
| **W3** | **CLOSED 2026-11-06** — the `Prebuild WebRTC APM` step used to run with `continue-on-error: true` + a per-platform ON/OFF fallback (×4); it is now a hard gate and Configure is pinned to `ON` | `.github/workflows/release.yml` (four `apm_prebuild` steps + four Configure blocks) | The meson prebuild of webrtc_audio_processing failed on windows-2022 at the v0.14.0 cut (abseil subproject ninja failure, `dc15ad8`), and the persistent Windows stub-3A outcome through `v1.0.2` additionally came from three defects in `tools/build_webrtc_apm.cmd` (LF-only line endings mis-parsed by cmd.exe, a `powershell -Command` wrapper turning ninja's stderr progress line into a terminating error, and a ninja call without `call`) — all three fixed in `9c3c522`, i.e. before the `v1.0.2` cut; the `Prebuild WebRTC APM` step of the final `v1.0.2` release run therefore **succeeded** on all four platforms, which is the removal condition this row asked for. **Closed 2026-11-06:** `continue-on-error` is dropped from all four legs and each Configure block passes a literal `-DNIMRTC_VENDORED_WEBRTC_APM:BOOL=ON`, so every bundle ships the real WebRTC APM 3A (AEC/ANS/AGC) and a prebuild regression aborts the cut instead of degrading the bundle to the built-in stub 3A path. `build/3a-status-<platform>.txt` is still written per leg (now always `3a=ON`) and the release body still reports the per-platform status; the record remains a *build-time* fact only — whether the published bundle carries a usable 3A depends on the install rules shipping the APM archives under `lib/webrtc_apm/` (`cmake/NimRTCInstall.cmake`, Track D — landed after `v1.0.2`), so the release body lists the tarball and reports the build mode and the shipped content separately. | A prebuild regression now aborts the release cut instead of shipping a bundle with the stub 3A path (the `v0.14.0` windows abort is the reference case). | **Closed 2026-11-06** — the prebuild is green on all four platforms (evidence: the final `v1.0.2` release run), so `continue-on-error` is dropped and Configure is pinned to `ON`; this row is retained as history. | No — closed |
| **W4** | Vendor manifest check is a hard gate (v1.0+) | `.github/workflows/ci.yml` (×4), `.github/workflows/interop.yml` (×2) | The drift gate (`tools/check_vendor.py`) only runs when `src/third_party/vendor.json` exists. v1.0+: drift = red CI = blocked merge (verified 9/9 match locally before the flip). | Unrelated PRs start failing on vendor drift. | **Removed 2026-10-30** — gate flipped hard for v1.0.0. | No |
| **W5** | `build-linux-aarch64` hard gate in `publish-release`'s `needs:` | `.github/workflows/release.yml` | **Original rationale was wrong.** The entry used to read "GitHub-hosted ARM64 runner capacity is unreliable — a real observed failure mode was a 14+ hour queue (run #35049874137, 2026-09-16 → 2026-10-14)". The queue was not a capacity problem: the runner label was `ubuntu-22.04-arm64`, which is **not a GitHub-hosted label at all**, so the job could never be allocated a runner (see §4). With the label corrected to `ubuntu-24.04-arm` this leg runs like the other three. **Closed 2026-11-05 (review):** the `v1.0.2` release published the aarch64 bundle, which counts as the first observed green run of the *release* aarch64 leg — the `continue-on-error: true` is dropped and the job is back in `publish-release`'s `needs:`. | A release cut that includes an aarch64 bundle blocks on that bundle; if the aarch64 release build has a real defect, the release is held rather than shipping a known-bad or partial artefact. | **Closed 2026-11-05** — gate flipped hard per review; this row is retained as history. | **See §4 — this is a support-matrix question** |
| **W6** | CI auto-retry: a job that never received a runner is re-run automatically | `.github/workflows/ci-retry.yml` + `tools/ci_retry_runner.py` | A GitHub-hosted runner is not guaranteed. A job asking for a saturated pool — the `macos-15` arm64 image is the chronic case — can be starved of a runner until the platform gives up ("The job was not acquired by Runner of type hosted even after multiple attempts"), ending `cancelled` with **no steps** and **no runner**. The run's conclusion is then `failure`, and the merge gate is red for a reason that has nothing to do with the tree. Measured on 2026-11-16: PRs #65, #66 and #67 each needed a manual `gh run rerun` before the macOS leg went green. The retry is re-run **only** when `steps == 0` **and** `runner_name == ""`; a job that *ran* and failed is never re-run, because that failure belongs to the tree and re-running it would hide the defect the CI run was for. The two signatures are disjoint and were verified against real runs (43938323684 → starved; 43877514623 → genuine). The same rule covers `cancelled`: over the 60 cancelled runs measured on 2026-11-19 there were 124 cancelled jobs that had a runner and had run steps (a `cancel-in-progress: true` supersede) against 25 that never reached a machine, so blanket-retrying `cancelled` would re-run 5× more superseded jobs than starved ones. **Revised 2026-11-19 (post-v1.2.0 review), two defects found by reading the ten red `ci-retry` runs:** (a) the loop guard was the run's `run_attempt`, which is wrong in both directions — a job-level re-run bumps it for *every* job, and run 44306330102 re-starved its macOS leg in attempt 2 and was refused, leaving the merge gate red for 39 minutes. The guard is now per job name (`--max-retries 1`, counted by walking the per-attempt job lists); verified by dry-running the classifier against 44306330102, which now selects the macOS leg where the old guard selected nothing. (b) Nine of the ten red runs were `403 Forbidden` from the re-run endpoint, not misclassifications: `workflow_run` fires on the *run* transition, but a concurrency cancel tears jobs down asynchronously and the run's own state is a snapshot taken at a different moment from its jobs' — run 44439751037 was still `in_progress` at 01:49:22Z when `ci-retry` fired and tried to re-run its attempt-1 jobs, and the platform refuses to re-run a job whose run is still running. The script now polls until the run reports `completed` (`--wait-polls 20 --wait-interval 15`), and if it never settles it says so and exits 0 rather than attempting the doomed call. The tenth was `HTTP 502: Bad Gateway`, a platform hiccup the script now retries with exponential backoff for 5xx/429 (403 is deliberately not retried — the same call gets the same answer). Scope is `ci`, `ci-e2e` and `release`; `ci-fast` was dropped because it triggered 1 of 498 measured `ci-retry` runs and `windows / fast` is not a required check; `soak` is excluded because both its jobs carry `continue-on-error: true` (a job that ran and failed still reports `success`, so the classifier would never touch it) and because the soak window is a statement about *calendar* days — a retry landing on the next UTC day is a second data point, not a replacement. | A starved job leaves the merge gate (or a release cut) red until somebody notices and re-runs it by hand; CI is unattended only for the runs that happen to be watched. | GitHub-hosted runner starvation stops happening at this rate, or the platform gains a first-class "retry this job" trigger. Until then the guard is cheap and safe. | No |

---

## 1. Vendor-state cleanliness (W1, W2)

W2 is still applied from `src/third_party/CMakeLists.txt` at
**configure time** (a `/wd28020` flag scoped to msquic's `core` target).
W1 moved out of the CMake overlay and into a committed change on a
fork, so its status is "commit in the fork" rather than "file rewrite
at configure time".  Consequences worth stating explicitly:

- `src/third_party/msquic` stays vendor-clean.  Its recorded HEAD is
  upstream `microsoft/msquic` `42376ae` plus one NimRTC commit
  (`a5ae3a06e`); the *whole* of that combined history lives on the
  `NimRTC/msquic` fork's `nimrtc-patches` branch, so the pin is fetchable
  from a fresh clone (no local-only commits).  `sh scripts/check-release-freeze.sh --only submodules`
  verifies reachability via `git -C src/third_party/msquic branch -r
  --contains <sha>` and must stay green — an unpinned or local-only
  commit breaks every clone but the author's.  (This is not
  hypothetical: the v0.14.0-rc.1 cycle pinned msquic at a local-only
  commit in `a88383e` and had to be reverted in `ef6ff11`; the fork is
  exactly the change that prevents the same failure mode from recurring.)
- W2's approach (configure-time flag injection) is unchanged, so the
  idempotency note below applies only to W2 now; the W1 marker string
  `OR TRUE)  # NimRTC: force dummy raw datapath` lives in a committed
  file and is no longer rewritten per-configure.

**Functional consequence to be honest about in release notes:** on
Windows the WebTransport / QUIC data path runs on msquic's dummy raw
datapath, not the XDP-backed one. This affects throughput and syscall
behaviour, not correctness. The Chrome e2e Case E harness
(`tests/chrome_interop_harness/`) exercises the dummy path, which is the
path that ships.

---

## 2. What is *not* a workaround

For the avoidance of doubt, these are real, tracked work — not
accommodations — and are deliberately absent from the register above:

| Item | Tracked in |
|---|---|
| Vendor header warnings suppressed for GCC/Clang (`SYSTEM` include dirs on `nimrtc_vendor_msquic`, `23a39e7`) | That is the correct contract for a vendored library, not a workaround. NimRTC's `-Werror` set applies only to NimRTC-owned code. |
| `-Wsign-conversion` fix in `MsQuicBidiStream::recv()` (`23a39e7`) | An ordinary code fix in NimRTC-owned source. |
| `msquic.dll` copy skipped on non-Windows (`98f80ea`) | Ordinary platform-conditional logic. |

---

## 3. W3 is closed — the APM prebuild is a hard gate

**Closed 2026-11-06.** The `audio3a` PUBLIC link fix has landed, so
abseil includes propagate when the prebuild succeeds. The workflow no
longer disables the step: each platform runs `Prebuild WebRTC APM` as a
hard gate and Configure is pinned to a literal `ON`:

```yaml
- name: Prebuild WebRTC APM
  id: apm_prebuild
  run: python tools/fetch_webrtc_apm.py
...
# Configure: -DNIMRTC_VENDORED_WEBRTC_APM:BOOL=ON (literal, no fallback)
```

A meson/abseil prebuild failure therefore **aborts the release cut**
instead of degrading the bundle to NimRTC's built-in stub 3A path. The
release always ships the real WebRTC APM 3A (AEC/ANS/AGC) on all four
platforms, and the per-platform mode is still recorded in
`build/3a-status-*.txt` (now always `3a=ON`) and surfaced in the
release notes. `CI` (always `OFF`) and `release` (always `ON`) still
diverge here — CI stays cheap because it never prebuilds the APM — but
the release side no longer has a degraded mode.

**The build-time record is not the shipped artefact.** `3a-status-*.txt`
says the APM was compiled into that build's NimRTC libraries; whether
the consumer's tarball carries a usable 3A additionally depends on the
install rules shipping the APM archives under `lib/webrtc_apm/`
(`cmake/NimRTCInstall.cmake`, Track D). The `v1.0.2` bundles predate
those install rules, so none of them contains `lib/webrtc_apm/` even
though the prebuild succeeded on all four platforms — and the `v1.0.2`
release body, which derived the 3A table from the build-time record
alone, claimed "real 3A bundled" for three platforms and blamed a
failed prebuild for the fourth. The release body now lists the
published tarball and reports the build mode and the shipped content
separately.

**Correction (2026-11-04): the `windows-2022` leg is not a known-failing
leg.** It was described as one because of the `v0.14.0` cut, where
meson/abseil genuinely failed to compile
`libwebrtc-audio-processing-2.a` (`dc15ad8`). But the *persistent*
Windows stub-3A outcome through `v1.0.2` came from three defects in
`tools/build_webrtc_apm.cmd` instead: LF-only line endings that cmd.exe
mis-parses, a `powershell -Command` wrapper that turns ninja's stderr
progress line into a terminating error, and a ninja invocation without
`call`. All three are fixed in `9c3c522` (and `.gitattributes` now pins
`*.cmd` / `*.bat` to CRLF), so the `windows` leg is expected to
prebuild the real APM. A Windows prebuild failure is now a defect to
fix, not a platform limitation to document.

**Evidence that closed the row:** the `Prebuild WebRTC APM` step of
the final `v1.0.2` release run **succeeded on all four platforms**
(the three Windows defects above were fixed in `9c3c522`, i.e. before
that cut). That is exactly the removal condition the register asked
for, so `continue-on-error` is dropped and Configure is pinned to `ON`
on all four legs. `RELEASING.md` §3.1 and §7.1 describe the hard-gate
contract, so the doc and the YAML agree.

---

## 4. W5 / the aarch64 support-matrix question

This is `docs/plan/v1.0-plan.md` §3 DoD **Gate #8**, restated in terms of
what the workflow actually does.

### 4.1 The root cause was a wrong runner label, not runner capacity

Everything this section used to say about "the GitHub-hosted ARM64 pool
is unreliable" was a **misdiagnosis**. The ARM64 leg never failed
because the pool was saturated. It never failed *at all* — it was
**unschedulable**, because the label it asked for was not a label that
exists.

`ubuntu-22.04-arm64` was introduced by commit `4467cd8` (2026-09-11).
GitHub's published runner-label set (`actions/runner-images` README,
"Available Images" table) lists exactly three Linux ARM64 labels:

| Image | Architecture | YAML Label |
|---|---|---|
| Ubuntu 26.04 Arm64 | arm64 | `ubuntu-26.04-arm` |
| Ubuntu 24.04 Arm64 | arm64 | `ubuntu-24.04-arm` |
| Ubuntu 22.04 Arm64 | arm64 | `ubuntu-22.04-arm` |

There is no `-arm64` suffix variant. (The `-arm64` string does appear
in that README, but only in the *image-directory* link targets —
`Ubuntu2204-Arm64-Readme.md` — never in the "YAML Label" column.) The
project's own earlier history used the valid label: `21e883f` and
`70e9de1` both requested `ubuntu-22.04-arm`, and `4467cd8` is the
commit that changed it to `ubuntu-22.04-arm64`.

A job that requests a label matching no runner does not fail — it sits
in `queued` **forever**. The check-run stays `queued`, `started_at`
equals `created_at`, and `runner_name` stays empty. Two independent
observations confirm exactly this signature, and neither of them is
what pool saturation looks like:

- **Run #269** (`#40370283784`, 2026-10-26). Its aarch64 job reports
  `labels: [ubuntu-22.04-arm64]` with an **empty `runner_name`**, and
  `started_at == created_at == 17:37:02Z`. The other three jobs in the
  *same run* — `macos-clang / arm64`, `windows / msvc`,
  `linux-gcc / x86_64` — were each allocated a runner within **five
  seconds** of creation. A saturated pool cannot be selective like
  that; a bad label affects exactly one leg and no other.
- **Commit `4467cd8`**'s aarch64 check-run sat queued for exactly
  **24 hours** (`2026-09-11T02:30:03Z` → `2026-09-12T02:30:03Z`) and
  was then cancelled. 24 h is the point at which the platform gives
  up on a job it can never schedule — not a point at which a busy
  queue drains.

So "the pool queued for 11+ hours", "the pool queued for 3+ hours"
and "run #268's job never received a runner at all" are all the same
single bug observed three times. The project then built an elaborate
narrative on top of it — a TEMP-DISABLE, then a `vars.NIMRTC_ARM64_CI`
repository-variable switch, then a support-matrix decision about
whether to ship 3 platforms or 4 — all of which was treating a
one-character typo in a label as an infrastructure capacity problem.

### 4.2 Current factual state, as of 2026-11-05 (W5 closure — see §4.3)

| Leg | `ci.yml` (build + unit tests) | `release.yml` (shipped artefact) |
|---|---|---|
| windows-2022 / MSVC | active, hard gate | active, hard gate |
| ubuntu-22.04 / GCC x86_64 | active, hard gate | active, hard gate |
| macos-14 / Apple Clang arm64 | active, hard gate | active, hard gate |
| ubuntu-24.04-arm / GCC | **active, hard gate** — no `if:`, no variable | **active, hard gate** — no `continue-on-error`, in `publish-release`'s `needs:` (W5 closed §4.3) |

**What changed on 2026-10-28.** Three things:

1. The runner label is **`ubuntu-24.04-arm`** — a real label, in the
   documented set. The `release.yml` leg moved with it.
2. The `linux-aarch64` job in `ci.yml` is a **plain hard gate again**.
   The `if: ${{ vars.NIMRTC_ARM64_CI == 'on' }}` condition and the
   `NIMRTC_ARM64_CI` repository variable are both gone. That variable
   was a workaround for a non-existent problem, and it had a second
   cost that was easy to overlook: with the variable unset the leg
   was *always* skipped, so the 4-platform matrix could never be
   observed green on a normal push — which is precisely what Gate #8
   asks for. `workflow_dispatch` is kept, since re-running the matrix
   on demand is useful regardless.
3. The `linux-aarch64` job no longer installs the
   `g++-aarch64-linux-gnu` cross toolchain.  The runner is a *native*
   arm64 host, and `cmake/toolchains/aarch64-linux-gnu.cmake` has a
   native branch (`uname -m == aarch64` → use the system compiler);
   installing the cross package would have made the toolchain file
   prefer the cross compiler and point `CMAKE_SYSROOT` at a sysroot
   the native image does not carry.

If the ARM64 leg ever has to be skipped again, use a code change (as
`34a6ace` / `d2152ff` did) rather than a repository variable. A
variable gate makes "is the ARM64 leg actually running?" a question
that must be answered out of band — which is the failure mode this
register exists to prevent.

### 4.3 Closed 2026-11-05: aarch64 is a hard gate again

The **CI** leg and the **release** leg are different jobs with
different build settings (`debug.aarch64` + `ctest` versus
`release.aarch64` + packaging). The CI leg has been observed green, and
the `v1.0.2` release published the aarch64 bundle — which counts as the
first observed green run of the *release* leg with the corrected label.
So `build-linux-aarch64` in `release.yml` no longer carries
`continue-on-error: true` and is back in `publish-release`'s `needs:` —
on the review evidence (2026-11-05), **not** on the old ground that the
runner pool might be busy.

That leaves no documentation gap on bundle coverage: `README.md`
§Platform support says:

> | Linux | aarch64 | ✅ Supported (compile-green; deployment validate on target hardware) |

which is honest about compile-green vs. deployment. The `dist/` bundle scan
still prints a "Partial release" banner when any bundle is missing, so a
regression there stays visible instead of silent.

**Gate #8 decision (closed 2026-11-05): ship aarch64 as a supported
platform.** `build-linux-aarch64` is a hard gate again (no
`continue-on-error`, back in `needs:`), and the README ✅ stays.

---

## 5. Release-bundle obligations (post-v1.2.0 review)

Two things the v1.2.0 review raised belong together, because the same
number exposed both: the Windows bundle grew from **40.5 MB at v1.1**
to **62,826,812 bytes (59.9 MiB) at v1.2.0**, a 55 % jump read
correctly as "seven new static codecs entered the MSVC line".

### 5.1 The licence texts did not ship (fixed 2026-11-19)

The review asked whether the licence obligations were complete. They
were not. `release.yml` writes a CycloneDX SBOM from
`src/third_party/vendor.json`, and that SBOM names each component's
SPDX id — but the licence **text** stayed in the source tree. Both
BSD-3-Clause and Apache-2.0 require the notice to accompany a binary
redistribution, so the bundle shipped six static codec libraries
(`libvpx`, `svt-av1`, `libyuv`, `dav1d`, `openh264`, `wolfssl`) with
the obligation written down but not discharged.

`tools/collect_licenses.py` now runs in all four release build jobs
after `cmake --install` and before packing. Per vendor in
`vendor.json` it:

| Case | Behaviour |
|---|---|
| Licence file at the tree root or `src/` (`LICENSE`, `LICENSE.txt`, `COPYING`, `NOTICE`, `PATENTS`, …) | copied to `licenses/<vendor>/`, SHA-256 recorded in `licenses/THIRD_PARTY_NOTICES.txt` |
| Notice embedded in every source file (`nv_codec_headers`) | the claim is **checked** (every header must carry the MIT notice), then the headers are carried under their original `include/ffnvcodec/` paths |
| Empty submodule mount point (not initialised in this clone) | skipped with a note — a local-clone fact, not a repo defect |
| Fetched at build time (`webrtc_audio_processing`) | skipped with a distinct note when absent locally |
| Distributed vendor with **no** licence file | **fails the release cut** |
| Test-only vendor (`NON_DISTRIBUTED`) | reported, does not fail the cut |

Checked against the pinned SHAs: **all 18 vendors** have a licence
obligation that can be discharged — 17 ship a licence file or an inline
notice, and `webrtc_audio_processing` ships one in the tree the build
fetches. Two specifics worth recording:

- **`wolfssl`** is dual-licensed `GPL-2.0-only OR (Apache-2.0 AND MIT)`;
  NimRTC relies on the Apache/MIT arm, which is why a GPL dependency
  does not make the bundle non-redistributable.
- **`libopus`** additionally ships `LICENSE_PLEASE_READ.txt` — a patent
  licensing statement that is part of its obligation, not a courtesy.

Three defects were found and fixed while wiring this gate, each of
which would have made it either useless or a false alarm:

1. The emptiness test ran on `<vendor>/`, but the wrapper directory
   keeps NimRTC's own `CMakeLists.txt`, so an uninitialised submodule
   never looked empty.  It now tests the submodule mount point
   (`vendor.json`'s `submodule_path`).
2. `dav1d`'s licence is `COPYING` at the **submodule root**
   (`<vendor>/src/COPYING`), not `<vendor>/src/LICENSE`.  The gate would
   have failed the next cut on a vendor that is fully licensed.  A
   `--self-test` case now pins this layout.
3. `nv_codec_headers` keeps its headers under `include/ffnvcodec/`, not
   at the vendor root, so the inline-notice check — and the copy — were
   looking in the wrong place.

`--self-test` pins the four cases above; it is registered in CTest as
`test_collect_licenses`.

### 5.2 The 64 MiB bundle limit was one codec away from failing (fixed 2026-11-19)

`tools/make_release_bundle.py` refused any bundle above **64 MiB**.
That limit was written when a real bundle was 2-3 MB and its job was to
catch the v1.0.1 mis-archive (the 822 MB asset that contained the whole
CI workspace). At 59.9 MiB the Windows payload had **6.4 % of headroom
left**, so the next static codec would have failed the cut with an
error that says "sanity limit" — a release blocker disguised as a
sanity check.

Two numbers now:

- `MAX_BUNDLE_BYTES = 256 MiB` — back to being a mis-archive detector;
- `BUNDLE_BUDGET_BYTES = 96 MiB` — a warning threshold that prints the
  current size, so payload growth shows up in the release log before it
  becomes a failure.

The payload itself is unchanged: no codec was removed.

---

## 6. Repository hygiene (post-v1.2.0 review)

| Item | Finding | Action |
|---|---|---|
| Stale remote branch `release/zz-ruleset-push-test` | Cut on 2026-11-14 to prove the `release/*` ruleset refuses deletion; already merged into `main`, so it was pure debris. The `deletion` rule (correctly) refused the delete — `bypass_actors` is empty and `Repository roles: admin` is deliberately not in it. | Set the ruleset to `disabled`, deleted the ref, set it back to `active`. Verified afterwards: the branch is `404`, the ruleset is `active`, and `release/1.2` reports `protected: true`. `python tools/apply_ci_ruleset.py` confirms all three mirrors still match. |
| `probe-*` workflow runs | No probe workflow exists in `.github/workflows/`; nothing to remove. | None. |
| Every other remote branch | 20 branches, all with open or in-flight work lines (`feat/q6-video-render-clock`, `feat/remote-desktop-m1-m4`, `docs/*`, `version/v1`–`v5`, …). Deleting them is a judgement call for their owners, not hygiene. | Not touched. |
| `required_approving_review_count: 0` on `main` | The review is right that 0 approvals is weak, but it is not a misconfiguration: this is a single-collaborator repository with no second account that can approve. `require_extra_approval_for_unattributed_changes` only bites once an approval exists, so it never fires either. | Documented in `.github/rulesets/protect-main.json`'s `_comment`: what actually gates `main` is the twelve required checks plus `strict_required_status_checks_policy`, not a human reviewer. Raising this to 1 requires a second approving account; that is a process change, recorded not silently "fixed". |

---

## 7. What a release preflight depends on

`release.yml`'s `preflight` job is the only thing that has to succeed
before the four build legs start.  It runs on `ubuntu-latest` with a
full-history checkout (`submodules: recursive`) and needs **no secrets
to pass** — the GPG secrets are read into an output and only turn
signing on; their absence is a warning, not a failure.

| Dependency | Why | What happens without it |
|---|---|---|
| The tag exists **on origin**, and is an **annotated, signed** tag | `workflow_dispatch` takes a tag name and checks it out by ref; the shape check reads the tag *object* | `git fetch`/checkout fails, or `verify_tag_signature.py --shape-only` fails |
| The tag is an **ancestor of `main`** | provenance (see §1) | the `merge-base --is-ancestor` step fails; merge the release branch into `main` first |
| `CMakeLists.txt` `project(... VERSION)` equals the tag | the package version and the tag must not drift | version-match step fails |
| **7 consecutive green soak days** on both tracked platforms | the soak gate | fails; it does **not** fall back to "no evidence" |
| Those soak records are **artifacts, not local files**, so they need `actions: read` on `GITHUB_TOKEN` | `fetch_soak_evidence.py` walks the `soak` workflow's runs and downloads their artifacts | the fetch step fails with an API error |
| Every daily record's `commit` is an **ancestor of the tag**, and the window closed within `NIMRTC_SOAK_MAX_AGE_DAYS` (30) | a window measured on another branch, or from two generations ago, is not evidence about this tree | `run_soak.py --require` fails |
| **Soak artifact retention ≥ the window read** | the fetcher reads `--days 21`; anything older has expired | the window is incomplete and the gate fails |
| **`node`** on the runner (for the API-symbols part of the freeze checklist) | `scripts/check-release-freeze.sh` shells out to node | `SKIP` in CI, a hard failure under `--final-tag` |
| Network access to the submodule remotes | the freeze checklist asks each pin's server whether the SHA is fetchable (`git fetch --dry-run`) | the `submodules` check fails |
| The README already describing **this** tag | `check_readme_release.py --expect-tag` | fails, so a release cannot ship with last version's front page |
| A clean working tree / initialised submodules | the freeze checklist's vendor and root checks | `FAIL vendor` / `FAIL root` |

Things it deliberately does **not** depend on:

- The GPG private key — pre-1.0 releases can be cut without it (a
  warning, and the body carries the unsigned banner).
- A second approving reviewer — `main` requires none (see §6).
- Any local build — preflight is entirely read-only over the repository
  and the Actions API.

The four build jobs then need what any build needs: the vendored
submodules (checked out recursively), the APM prebuild (`meson` via
`tools/fetch_webrtc_apm.py`), a C++20 toolchain, Python 3 for the
tooling, and `xz`/`tar` on the Linux/macOS legs.  Each build job also
runs `tools/collect_licenses.py` after `cmake --install`; if a
distributed vendor's tree has no licence file the **build** job fails,
which is inside the release but after preflight.

---

## 8. Maintenance

- New workaround → add a row before the commit lands, not after CI goes
  red and gets patched in place.
- Removing a workaround → delete the row and note the removal commit in
  `CHANGELOG.md` under the release that dropped it.
- `RELEASING.md` §3.1 step 5 runs `sh scripts/check-release-freeze.sh`
  as the mechanical half of this register's enforcement (SFU-track
  leakage, header classification, pre-release tag, submodule
  reachability, root cleanliness). This register covers the
  *judgement* items that a script cannot assert.