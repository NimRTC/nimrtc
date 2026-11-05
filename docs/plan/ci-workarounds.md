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
| Blocks v1.0.0? | **W1 and W2 do not.** **W3 does** (see table) |

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
| **W3** | `Prebuild WebRTC APM` runs with `continue-on-error: true` + per-platform ON/OFF fallback (×4) | `.github/workflows/release.yml` (four `apm_prebuild` steps + four Configure blocks reading `steps.apm_prebuild.outcome`) | The meson prebuild of webrtc_audio_processing failed on windows-2022 at the v0.14.0 cut (abseil subproject ninja failure, `dc15ad8`), and the persistent Windows stub-3A outcome through `v1.0.2` additionally came from three defects in `tools/build_webrtc_apm.cmd` (LF-only line endings mis-parsed by cmd.exe, a `powershell -Command` wrapper turning ninja's stderr progress line into a terminating error, and a ninja call without `call`) — all three fixed in `9c3c522`, i.e. before the `v1.0.2` cut; the `Prebuild WebRTC APM` step of the final `v1.0.2` release run therefore **succeeded** on all four platforms. The step runs but may fail; Configure picks `ON` on success and `OFF` on failure, recording the outcome in `build/3a-status-*.txt`, so the release is never aborted by the prebuild. `audio3a` already links `nimrtc_vendor_webrtc_apm` PUBLIC so abseil includes propagate when ON. The record is a *build-time* fact only: whether the published bundle carries a usable 3A depends on the install rules shipping the APM archives under `lib/webrtc_apm/` (`cmake/NimRTCInstall.cmake`, Track D — landed after `v1.0.2`), so the release body lists the tarball and reports the build mode and the shipped content separately. | Forcing ON unconditionally would let a genuine prebuild failure abort the release cut (the `v0.14.0` windows abort). | Windows prebuild goes green reliably; then drop `continue-on-error` and pin Configure to `ON`. | **See §3 — per-platform 3A variance, recorded in release notes** |
| **W4** | Vendor manifest check is a hard gate (v1.0+) | `.github/workflows/ci.yml` (×4), `.github/workflows/interop.yml` (×2) | The drift gate (`tools/check_vendor.py`) only runs when `src/third_party/vendor.json` exists. v1.0+: drift = red CI = blocked merge (verified 9/9 match locally before the flip). | Unrelated PRs start failing on vendor drift. | **Removed 2026-10-30** — gate flipped hard for v1.0.0. | No |
| **W5** | `build-linux-aarch64` hard gate in `publish-release`'s `needs:` | `.github/workflows/release.yml` | **Original rationale was wrong.** The entry used to read "GitHub-hosted ARM64 runner capacity is unreliable — a real observed failure mode was a 14+ hour queue (run #35049874137, 2026-09-16 → 2026-10-14)". The queue was not a capacity problem: the runner label was `ubuntu-22.04-arm64`, which is **not a GitHub-hosted label at all**, so the job could never be allocated a runner (see §4). With the label corrected to `ubuntu-24.04-arm` this leg runs like the other three. **Closed 2026-11-05 (review):** the `v1.0.2` release published the aarch64 bundle, which counts as the first observed green run of the *release* aarch64 leg — the `continue-on-error: true` is dropped and the job is back in `publish-release`'s `needs:`. | A release cut that includes an aarch64 bundle blocks on that bundle; if the aarch64 release build has a real defect, the release is held rather than shipping a known-bad or partial artefact. | **Closed 2026-11-05** — gate flipped hard per review; this row is retained as history. | **See §4 — this is a support-matrix question** |

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

## 3. W3 is a per-platform 3A variance, handled by degraded release

The `audio3a` PUBLIC link fix has landed, so abseil includes propagate
when the prebuild succeeds. The workflow no longer disables the step;
each platform runs `Prebuild WebRTC APM` with `continue-on-error: true`
and Configure picks `ON`/`OFF` from `steps.apm_prebuild.outcome`:

```yaml
- name: Prebuild WebRTC APM
  id: apm_prebuild
  continue-on-error: true
  run: python tools/fetch_webrtc_apm.py
...
# Configure: apm="ON" iff prebuild succeeded, else "OFF" (stub 3A)
```

So a bundle may or may not carry the real WebRTC APM 3A depending on
whether the meson/abseil prebuild succeeded on that runner. The
per-platform outcome is recorded in `build/3a-status-*.txt` and
surfaced in the release notes. `CI` (always `OFF`) and `release`
(best-effort `ON`) deliberately diverge here; the release is never
aborted by a prebuild failure.

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

**Remaining work before the v1.0.0 tag:** update `RELEASING.md` §3.1
to describe this degraded contract (instead of unconditional `ON`),
so the doc and the YAML agree. The full fix — windows prebuild green,
then drop `continue-on-error` and pin Configure to `ON` — is the W3
removal condition in the table above.

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

## 5. Maintenance

- New workaround → add a row before the commit lands, not after CI goes
  red and gets patched in place.
- Removing a workaround → delete the row and note the removal commit in
  `CHANGELOG.md` under the release that dropped it.
- `RELEASING.md` §3.1 step 5 runs `sh scripts/check-release-freeze.sh`
  as the mechanical half of this register's enforcement (SFU-track
  leakage, header classification, pre-release tag, submodule
  reachability, root cleanliness). This register covers the
  *judgement* items that a script cannot assert.