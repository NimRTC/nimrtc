# Releasing NimRTC

This document describes how NimRTC versions its releases, tags source-tree
milestones, and ships binaries. It is the single source of truth for release
managers and the source for the §16.3 DoD checklist in
`docs/zh/architecture.md`.

If you are looking for how to **contribute** a change, see
[`CONTRIBUTING.md`](CONTRIBUTING.md) instead. This document is about how a
change becomes a numbered, tagged artefact that other people depend on.

---

## 1. Versioning scheme (SemVer 2.0.0)

NimRTC follows [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html).
Every release is identified by a three-part `MAJOR.MINOR.PATCH` number with an
optional pre-release suffix:

```
MAJOR.MINOR.PATCH[-PRERELEASE][+BUILD]
```

| Component       | When it changes                                                                                                    |
|-----------------|---------------------------------------------------------------------------------------------------------------------|
| `MAJOR`         | Breaking changes to the **public API** (headers under `include/nimrtc/`, exported symbols, ABI). Resets `MINOR` and `PATCH` to `0`. |
| `MINOR`         | New backwards-compatible features or new modules. Resets `PATCH` to `0`.                                            |
| `PATCH`         | Backwards-compatible bug fixes, performance work, dependency bumps that preserve public-API surface.                |
| `PRERELEASE`    | Optional. See §2.                                                                                                  |
| `BUILD`         | Optional SemVer build metadata. Ignored by SemVer precedence; we use it only for local/non-distributed artefacts.  |

### 1.1 Public-API breaking changes

A change is **breaking** if it requires a downstream user to edit their code,
re-link against a different symbol set, or rebuild with new flags. Examples:

- Removing or renaming a header under `include/nimrtc/<module>/`
- Changing the signature of an exported function in a `nimrtc::*` namespace
- Changing the layout of a type passed by value across an ABI boundary
- Changing the on-the-wire format of any public codec (RTP/RTCP/SDP/DTLS/SRTP
  extension point) without an opt-in transition window
- Raising the minimum toolchain floor (C++ standard, CMake, MSVC/GCC/Clang
  major versions) by more than one major version

Pure-internal refactors that keep the public headers and exported symbols
identical are **not** breaking — they ship in `PATCH`.

### 1.2 Pre-1.0 special case

While `MAJOR == 0`, the SemVer spec says the API is not yet stable. NimRTC
treats `0.x.y` as "Tech Preview" — every `MINOR` bump may include breaking
changes, but every `PATCH` bump on a given `0.MINOR` line must stay backwards
compatible within that line. This matches the §13 release plan:

| Phase | Tag form          | Audience           | Stability promise                             |
|-------|-------------------|--------------------|-----------------------------------------------|
| P0    | `v0.1.0`          | internal only      | nothing stable                                |
| P1    | `v0.5.x`          | first public (TP)  | Tech Preview, no API guarantees               |
| P2    | `v0.7.x`          | public Beta        | Beta API, freezes before `v0.9.0`             |
| P3    | `v0.9.0-rc.N`     | RC + interop       | candidate API, frozen until `v1.0.0`          |
| P4    | `v1.0.0`          | first stable       | SemVer backwards compatibility from this tag  |

`v1.0.0` is the API-stability gate. From `v1.0.0` onward, `MAJOR` bumps only on
documented, communicated breaking changes per the schedule in §3.3.

---

## 2. Tag strategy

### 2.1 Tag naming

All release tags are prefixed with a literal `v`:

```
v<MAJOR>.<MINOR>.<PATCH>[-<PRERELEASE>]
```

Examples that already exist or are planned:

| Tag                 | Meaning                                                        |
|---------------------|----------------------------------------------------------------|
| `v0.1.0`            | P0 scaffold (internal)                                         |
| `v0.5.0`            | P1 public Tech Preview                                         |
| `v0.7.0`            | P2 public Beta                                                 |
| `v0.9.0-rc1`        | First release candidate toward `v1.0.0`                        |
| `v0.9.0-rc2`        | Subsequent release candidate (resets `rc.N`, not `MINOR`)      |
| `v0.9.0`            | Final `0.9.x` patch release (post-RC hot-fixes only)           |
| `v1.0.0`            | First API-stable release                                       |

### 2.2 Pre-release identifiers

NimRTC uses [SemVer pre-release identifiers](https://semver.org/#spec-item-9)
in two cases:

1. **Release candidates** — `v0.MINOR.0-rc.N` (or `v1.0.0-rc.N` for the
   first-stable gate). The integer `N` is incremented for each successive RC.
   Per SemVer, `v0.9.0-rc2 > v0.9.0-rc1 > v0.9.0-beta > v0.9.0-alpha`.
2. **Experimental milestones** (rare, internal-only) — `v0.MINOR.PATCH-<word>`
   where `<word>` ∈ {`alpha`, `beta`}. These are not advertised externally and
   must not be installed by `cmake --build` consumers via `find_package`.

Pre-release tags **must**:

- Carry an annotated tag object (`git tag -a v0.9.0-rc1 -m "..."`), not a
  lightweight tag, so `git describe` and `cmake --build` both pick up the
  annotated ref.
- Sign with the release manager's GPG/SSH key (`git tag -s ...` or
  `git tag -u <keyid> ...`) once the project enables tag signing.
- Be pushed explicitly with `git push origin v0.9.0-rc1` — pushing the branch
  alone does **not** propagate the tag.

### 2.3 Tag immutability

Once a tag is published to the public mirror:

- The commit it points at is **frozen**. Any further change to that line of
  development MUST go into a new tag (`v0.9.0-rc2`, `v0.9.1`, ...).
- Hot-fix commits land on a dedicated release branch (see §3.2) and the tag is
  re-pointed **only** before public announcement. After announcement, a fix
  becomes `v0.9.1`.

### 2.4 Branch ↔ tag mapping

| Branch        | Receives                            | Tags emitted          |
|---------------|-------------------------------------|-----------------------|
| `main`        | day-to-day development              | pre-release tags only (`*-rc.*`, `*-alpha.N`, `*-beta.N`) |
| `release/0.9` | stabilisation commits for `v0.9.x`  | `v0.9.0-rc.N`, `v0.9.N` |
| `release/1.0` | stabilisation commits for `v1.0.x`  | `v1.0.0-rc.N`, `v1.0.N` |

`release/X.Y` is cut from `main` immediately after the last feature PR merges
for that milestone. After `vX.Y.0` ships, only `PATCH`-level fixes land on
`release/X.Y`; new development continues on `main`.

---

## 3. Release process

The full release flow for any tag, including `v0.9.0-rc.N`, is below. Steps
3.4–3.9 are mandatory for the first stable `v1.0.0`; for pre-`1.0` RCs,
sections 3.5 (security audit) and 3.6 (external comms window) are skipped.

### 3.1 Pre-flight (T-7 days)

1. Confirm the milestone's DoD items in `docs/zh/architecture.md` §16.3
   and the matching release-plan table row are green.
2. Open a tracking issue titled `Release: vX.Y.Z` and assign the release
   manager.
3. Cut the `release/X.Y` branch from `main` per §2.4.
4. Run a full `cmake --build` + `ctest --preset tests.msvc` cycle on all four
   CI runners (windows, linux-gcc, linux-aarch64, macos-clang). All four must
   be green before proceeding.

   **CI vs release build differ on WebRTC APM**: `.github/workflows/ci.yml`
   passes `-DNIMRTC_VENDORED_WEBRTC_APM=OFF` to all four Configure steps so
   the runner doesn't need a `meson`+`abseil-cpp` prebuild. The actual release
   artefact built by `.github/workflows/release.yml` **must** carry real 3A,
   so the release pipeline runs `python tools/fetch_webrtc_apm.py` once per
   platform before Configure, then configures with `-DNIMRTC_VENDORED_WEBRTC_APM=ON`.
   See `docs/zh/architecture.md` §11.5.5 for the rationale.

### 3.2 Stabilisation window (T-7 … T-1)

5. Cherry-pick hot-fixes from `main` to `release/X.Y`. Each hot-fix PR must
   carry the `release-blocker` label and be reviewed by the module's
   `CODEOWNERS` owner (§16.3 DoD).
6. Update `CHANGELOG.md`: move entries from `## [Unreleased]` into a new
   `## [X.Y.Z] - YYYY-MM-DD` heading following
   [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).
7. Run `python tools/check_ci.py --release` (or equivalent) to verify:
   - `reuse lint` is clean (or a documented waiver exists in `cmake/REUSE.toml`).
   - `scancode-toolkit` reports no new licence conflicts vs. `NOTICE`.
   - Dependabot PRs are merged or explicitly deferred.
8. Bump the version in:
   - `CMakeLists.txt` (`project(NimRTC VERSION X.Y.Z ...)`)
   - `src/core/include/nimrtc/core/version.hpp`
   - The `Doxyfile`/`docs/api/` version banner (if present)
9. Smoke-test the example binaries: `examples/loopback-p2p` must complete a
   full ICE+DTLS+SRTP+RTP handshake against itself and exit 0.

### 3.3 Tag day (T)

10. Tag from `release/X.Y` HEAD:

    ```bash
    git checkout release/X.Y
    git pull --ff-only
    git tag -s -a vX.Y.Z -m "NimRTC vX.Y.Z"
    git push origin vX.Y.Z
    ```

11. The push triggers `.github/workflows/release.yml` which:
    - Builds the four-platform release artefacts (`nimrtc-{windows,linux,linux-aarch64,macos}.tar.xz`
      containing headers, CMake config files, and the static `.lib`/`.a`).
    - Signs the artefacts with the release manager's GPG key.
    - Generates the SBOM (`reuse` + `scancode-toolkit` + `cyclonedx-bom`).
    - Drafts a GitHub Release with `CHANGELOG.md`'s `## [X.Y.Z]` block as the
      body.

### 3.4 Post-tag (T+1 day)

12. Merge `release/X.Y` back into `main` (fast-forward) so the version bump
    is visible on `main` too. This merge commit is **not** a new release.
13. Open a PR titled `chore: post-release version bump` that advances
    `CMakeLists.txt` and `version.hpp` to `X.Y.(Z+1)-dev` on `main` so
    `find_package(NimRTC)` cannot accidentally pick up an in-progress build
    as `X.Y.Z`.
14. Mark the milestone as `100% complete` on GitHub.
15. Announce:
    - GitHub Release (auto-created in step 11) — pinned to the repo front page.
    - Project mailing list / Discussions thread for public RCs and stable
      releases only (not for `*-alpha.*` / `*-beta.*` internal tags).

### 3.5 Backports and security releases

Security fixes skip the normal cadence:

- A security advisory is published via GitHub Private Vulnerability Reporting
  (see `SECURITY.md`) **before** the tag is pushed.
- The fix lands on `main` and is cherry-picked to every supported
  `release/X.Y` branch listed in `SECURITY.md` ("Supported versions").
- A `vX.Y.(Z+1)` (or `vX.(Y+1).0`) tag is cut following §3.3 with an extra
  label `security` on the GitHub Release.

---

## 4. CMake / package manager integration

NimRTC installs CMake config files under `lib/cmake/NimRTC/NimRTCConfig.cmake`
plus a version file `lib/cmake/NimRTC/NimRTCConfigVersion.cmake` that uses the
standard `AnyNewerThan` / `SameMajorVersion` SemVer matching rules.

Downstream consumers can therefore write:

```cmake
find_package(NimRTC 1.0 REQUIRED)         # any 1.x
find_package(NimRTC 1.2.3 REQUIRED)       # exactly 1.2.3 or any 1.2.x
find_package(NimRTC 1.2 EXACT REQUIRED)   # exactly 1.2.0
```

Per SemVer:

- `find_package(NimRTC 1 REQUIRED)` accepts `v1.0.0`, `v1.2.3`, `v1.9.99` —
  but **not** `v2.0.0` or any `v0.x.y` (including `v0.9.0`).
- `find_package(NimRTC 0.9 REQUIRED)` accepts `v0.9.0` and `v0.9.7`, but **not**
  `v0.8.4` or `v1.0.0`.
- Pre-release tags (`v0.9.0-rc1`) are **not** matched by default. To opt in:
  `find_package(NimRTC 0.9.0-rc1 REQUIRED)` — CMake's version-file matching
  treats `-rcN` as a pre-release identifier per SemVer.

---

## 5. Deprecation policy

When a public API must be removed:

1. **Deprecate** in a `MINOR` release. The header keeps the symbol but adds
   `[[deprecated("Use nimrtc::foo::bar instead; will be removed in v2.0.0")]]`
   or the equivalent `NIMRTC_DEPRECATED(msg)` macro.
2. **Document** the deprecation in `CHANGELOG.md` under the same `MINOR`
   release with a clear migration path and the `MAJOR` version that will
   remove it.
3. **Remove** no earlier than the next `MAJOR` bump, and never within the
   same `MAJOR` line. `v0.x.y` may accelerate this for Tech Preview APIs (a
   `MINOR` bump is enough).
4. **Announce** the removal in `RELEASING.md`'s corresponding tag section
   (`§6`) at the start of the deprecation cycle, not just at removal time.

---

## 6. Per-tag notes

This section is appended at release time. Each entry cross-references the
`CHANGELOG.md` heading for the tag and links the GitHub Release.

| Tag            | Date       | Highlights                                          | GitHub Release |
|----------------|------------|-----------------------------------------------------|----------------|
| `v0.1.0`       | 2025-??    | P0 scaffold (§13 / §16.3)                           | TBD            |
| `v0.5.0`       | TBD        | P1 first public Tech Preview (Chrome ↔ NimRTC TP)   | TBD            |
| `v0.9.0-rc1`   | 2026-09-06 | RC1 — see `CHANGELOG.md` `[0.9.0-rc1]`; Case D interop **known issue** | TBD |
| `v0.9.0`       | TBD        | Final `0.9.x` line; only hot-fixes since `rc1`      | TBD            |
| `v1.0.0`       | TBD        | First API-stable release; SemVer guarantees apply   | TBD            |

---

## 7. Vendor migration status (as of 2026-09-11)

The `v1.0.0` tag requires the vendor migration to be complete per
`docs/plan/vendor-migration.md`. Current status:

| Phase | Description                                    | Status |
|-------|------------------------------------------------|--------|
| Phase 1 | Schema + manifest — `vendor.json` + `.gitmodules` | ✅ Done |
| Phase 2 | Tools — `check_vendor.py` + `vendor_update.py` | ✅ Done |
| Phase 3 | Submodule switch — one PR per vendor (8 PRs)   | ⬜ Pending |
| Phase 4 | CI gate — `check_vendor.py` in `ci.yml` / `interop.yml` | ✅ Done |
| Phase 5 | Remove `src/third_party/SOURCE_VERSIONS`      | ⬜ Pending |

Phase 3 and Phase 5 are blocked on the Phase 3 PRs merging. Until then,
`tools/check_vendor.py` is wired into CI as `continue-on-error: true`
(warning-only) per Phase 4 policy. Phase 5 is a one-line `git rm`
after Phase 3 completes.

### 7.1 WebRTC APM prebuild (non-migration note)

WebRTC APM is **not** part of the Phase 3 migration — it is already vendored
in `src/third_party/webrtc_audio_processing/` and built by meson (not CMake).
Its conditional prebuild guard (`NIMRTC_VENDORED_WEBRTC_APM`) is documented in
`docs/zh/architecture.md` §11.5.5. The short version: CI uses `=OFF`; the
release pipeline uses `=ON` after running `python tools/fetch_webrtc_apm.py`
once per platform.

---

## 8. Cross-references

- [`CONTRIBUTING.md`](CONTRIBUTING.md) — DCO signing, Conventional Commits,
  PR process.
- [`CHANGELOG.md`](CHANGELOG.md) — Keep a Changelog format, current
  `[Unreleased]` queue.
- [`SECURITY.md`](SECURITY.md) — vulnerability disclosure + supported-versions
  matrix used by §3.5.
- [`CODEOWNERS`](CODEOWNERS) — per-module reviewers who must approve
  release-blocker PRs.
- `docs/zh/architecture.md` §13 (release plan) and §16.3 (release
  hygiene DoD), and §11.5.5 (WebRTC APM conditional prebuild guard).
- `docs/plan/vendor-migration.md` — submodule + `vendor.json` migration that
  is a **blocker** for `v1.0.0` (see CHANGELOG "Deferred for 1.0.0").
