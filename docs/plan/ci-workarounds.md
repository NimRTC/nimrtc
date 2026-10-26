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
| **W3** | `Prebuild WebRTC APM` step disabled via `if: ${{ false }}` (×4) | `.github/workflows/release.yml` lines 186, 273, 350, 445 | The step's output was unused: the release Configure passes `-DNIMRTC_VENDORED_WEBRTC_APM=OFF`, so `audio3a` links the same system/stub APM path CI uses. Caused a `release / build / windows` failure during the v0.14.0 release cut. | Nothing today — the step is dead weight. Restoring it *without* also flipping the Configure flag would reintroduce the failure. | Make `target_link_libraries(audio3a PUBLIC webrtc_apm::webrtc_apm)` so abseil's `INCLUDE_DIRECTORIES` propagate to the consumer, then restore the `if:` line **and** configure with `NIMRTC_VENDORED_WEBRTC_APM=ON`. | **See §3 — this is a release-artifact gap** |
| **W4** | Vendor manifest check is `continue-on-error: true` | `.github/workflows/ci.yml` (×4), `.github/workflows/interop.yml` (×2) | The drift gate (`tools/check_vendor.py`) only runs when `src/third_party/vendor.json` exists. Warn-only pre-1.0 so an unrelated PR is not blocked by vendor bookkeeping. | Unrelated PRs start failing on vendor drift. | **Intentional through v0.x.** At v1.0.0 the check becomes a hard gate and `continue-on-error` is removed. | No (but see §3) |
| **W5** | `build-linux-aarch64` is `continue-on-error: true` and excluded from `publish-release`'s `needs:` | `.github/workflows/release.yml` lines 322, 505 | GitHub-hosted ARM64 runner capacity is unreliable — a real observed failure mode was a 14+ hour queue (run #35049874137, 2026-09-16 → 2026-10-14). Making it a hard gate would block the Windows / Linux x86_64 / macOS trio. | Either releases block for hours waiting for a runner, or aarch64 silently stops shipping. | GHA ARM64 runner capacity is reliably available. Until then the `dist/` bundle scan (lines 710-718) prints a "Partial release" banner when the aarch64 tarball is missing. | **See §4 — this is a support-matrix question** |

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

## 3. W3 is a release-artifact gap, not just a disabled step

This is the most consequential entry and is easy to misread as "CI
hygiene".

`RELEASING.md` §3.1 states the release contract:

> The actual release artefact built by `.github/workflows/release.yml`
> **must** carry real 3A, so the release pipeline runs
> `python tools/fetch_webrtc_apm.py` once per platform before Configure,
> then configures with `-DNIMRTC_VENDORED_WEBRTC_APM=ON`.

The workflow as it stands does the opposite on all four platforms:

```yaml
- name: Prebuild WebRTC APM
  if: ${{ false }}          # step 3.1 says this should run
  run: python tools/fetch_webrtc_apm.py
...
cmake --preset release.msvc ... -DNIMRTC_VENDORED_WEBRTC_APM=OFF   # §3.1 says ON
```

So released binaries ship **without** the vendored WebRTC APM 3A path
that the v1.0.0 support matrix claims. `CI` and `release` agree with each
other (both use `OFF`), so nothing goes red — the gap is between the
documented release contract and the workflow.

**Decision required before the v1.0.0 tag.** Three honest options:

1. **Ship without vendored APM in v1.0.0** and correct the contract in
   `RELEASING.md` §3.1 + the README 3A row to say the released artefact
   uses the stub APM path, with WebRTC APM documented as an integrator-
   supplied dependency. Smallest change; requires no CI work.
2. **Fix the include propagation** (`target_link_libraries(audio3a PUBLIC
   webrtc_apm::webrtc_apm)`), restore the step, flip all four Configure
   lines to `ON`. Real work: meson + abseil prebuild must go green on
   all four runners, and `RELEASING.md` §3.1's warning about CI-vs-release
   divergence becomes unnecessary.
3. **Ship v1.0.0 without APM and schedule (2) for v1.1.0**, recording the
   divergence in the v1.0.0 release notes under "Notes" so integrators
   are not surprised.

Option 1 or 3 makes the tree self-consistent immediately. Option 2 is
what the current documentation claims, but it is the only one that costs
real engineering time. **Whichever is chosen must be reflected in
`RELEASING.md`, not left as a contradiction between a doc and a YAML
file.**

---

## 4. W5 / the aarch64 support-matrix question

This is `docs/plan/v1.0-plan.md` §3 DoD **Gate #8**, restated in terms of
what the workflow actually does.

Current factual state, as of 2026-10-27:

| Leg | `ci.yml` (build + unit tests) | `release.yml` (shipped artefact) |
|---|---|---|
| windows-2022 / MSVC | active, hard gate | active, hard gate |
| ubuntu-22.04 / GCC x86_64 | active, hard gate | active, hard gate |
| macos-14 / Apple Clang arm64 | active, hard gate | active, hard gate |
| ubuntu-22.04-arm64 / GCC | ⏸ **TEMP-DISABLED** (`if: false` in the `linux-aarch64` job of `ci.yml`) — re-enabled 2026-10-14, disabled again 2026-10-27 | `continue-on-error: true`, excluded from `publish-release`'s `needs:` |

The `ci.yml` leg was active (and a hard gate) from 2026-10-14 to
2026-10-27.  The GitHub-hosted `ubuntu-22.04-arm64` pool then failed
again: CI run #269 (#40370283784) sat `queued` for 3+ hours without
allocation and run #268's aarch64 job never received a runner at all.
A job that never starts keeps the whole workflow run in `in_progress`,
so the run never reaches a terminal state and no merge or release gate
can observe it.  The leg is therefore TEMP-DISABLED again — the same
temporary mechanism used for 2026-09-16 → 2026-10-14 — so the other
three platforms can finish and the run can complete.  Re-enable by
removing the `if: false` line once the ARM64 pool is healthy; the
reason is recorded in `docs/plan/v0.13.0-release-notes.md` §4.

So the CHANGELOG prose "linux-aarch64 may be TEMP-DISABLED per v0.12.0
baseline" is accurate again: the CI leg is TEMP-DISABLED while the
runner pool is unusable, and the *release artefact* leg is the
permanently soft one.

That leaves a documentation gap rather than a build gap. `README.md`
§Platform support currently claims:

> | Linux | aarch64 | ✅ Supported (compile-green; deployment validate on target hardware) |

which is honest about compile-green vs. deployment, but does not say that
an aarch64 release tarball may be absent from a given release. The
`dist/` scan prints a "Partial release" banner when it is missing, so
the information exists — it just is not in the README.

**Gate #8 cannot be closed by CI alone.** The release manager must pick:

- **Ship aarch64 as a supported platform at v1.0.0** → make
  `build-linux-aarch64` a hard gate (drop `continue-on-error`, add it to
  `needs:`), accept the runner-queue risk, and keep the README ✅.
- **Ship v1.0.0 as 3-platform** → remove `ubuntu-22.04-arm64` from the
  README support table and the 4-platform CI badge, and say so in the
  release notes.

The current state — README says 4 platforms, release ships 3-plus-one-
maybe — is the one option that should not survive to the tag.

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