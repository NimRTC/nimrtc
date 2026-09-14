# Vendor migration plan — src/third_party/ → submodules + vendor.json

| | |
|---|---|
| Version | v1.0 (draft) |
| Date | 2026-09-11 |
| Status | **DRAFT — schema landed, SHA pinning pending** |
| Goal | Replace ~280 MB of checked-in vendor source with git submodules + a SHA-pinned `vendor.json` manifest, without breaking offline builds. |

## 1. Background

`src/third_party/` currently holds source trees for eight upstream
projects (libjuice, libsrtp, mbedtls, libopus, wolfssl,
webrtc_audio_processing, googletest, nlohmann_json). Total: ~317 MB
across ~11 600 files (measured 2026-09-11):

| Vendor                        | Size       | Files | Pinned? |
|-------------------------------|------------|-------|---------|
| wolfssl                       | 123 MB     | 3 622 | partial (v5.9.2, SHA unknown) |
| webrtc_audio_processing       | 121 MB     | 2 743 | partial (webrtc-m129, SHA unknown) |
| mbedtls                       |  37 MB     | 2 056 | ✅ (4.2.0 / `023aca8…`) |
| libopus                       |  29 MB     |   483 | ✅ (v1.6.1 / `7030c73…`) |
| googletest                    |   3.4 MB   |   205 | partial (v1.12.1) |
| libsrtp                       |   2.9 MB   | 2 398 | partial (master) |
| nlohmann_json                 |   0.9 MB   |     3 | partial (v3.11.3) |
| libjuice                      |   0.5 MB   |    78 | ✅ (master / `77daa8b…`) |

Source: `src/third_party/SOURCE_VERSIONS` + PowerShell `Get-ChildItem -Recurse`.

This was deliberately listed as a **1.0.0 blocker** in
`CHANGELOG.md` `[0.9.0-rc1]` ("Vendor sources are checked into the
tree… Migrating to submodules + a vendor.json manifest with SHA256-pinned
tags is a blocker for the 1.0.0 tag."). As of this draft, the manifest
schema and `.gitmodules` skeleton exist but no SHAs are pinned and no
migration has executed.

## 2. Goal & non-goals

### 2.1 Goal

By 1.0.0:

- All eight vendor trees replaced by git submodules pointing at pinned
  commits in `vendor.json`.
- `vendor.json` contains the canonical SHA for every submodule.
- `tools/check_vendor.py` fails CI if any submodule SHA drifts from the
  manifest.
- Upstream security patches applied via `git submodule update --remote
  <name>` + a one-line `vendor.json` bump.
- Net source-tree size drops from ~317 MB to <1 MB (just the submodule
  pointer files).

### 2.2 Non-goals

- No CI build of upstream sources from scratch (submodule update is
  expected to bring a pristine tree; `cmake --build` reuses cached
  objects).
- No removal of `src/third_party/SOURCE_VERSIONS` (it stays as a
  human-readable changelog).
- No removal of the historical mbedtls source tree (kept as archive;
  full removal is a separate PR per CHANGELOG `[0.9.0-rc1]`).
- No move to monorepo / vendor as npm-style package.

## 3. Migration phases

### Phase 1 — schema & manifest (DONE in this PR)

- ✅ Draft `src/third_party/vendor.json` schema.
- ✅ Draft `.gitmodules` listing every vendor with its branch.
- 🔲 Fill in missing SHAs (currently `REPLACE_WITH_FULL_SHA`):
    - wolfssl: fetch `v5.9.2` tag SHA from `https://github.com/wolfSSL/wolfssl`
    - webrtc_audio_processing: fetch `master` HEAD SHA from the PulseAudio mirror
    - googletest: `v1.12.1` tag SHA
    - nlohmann_json: `v3.11.3` tag SHA

**Exit criteria**: `vendor.json` validates as JSON, every vendor has a
40-character SHA, `.gitmodules` parses with `git config -f .gitmodules
--list`.

### Phase 2 — SHA collection script (1 day)

Add `tools/check_vendor.py`:

```python
#!/usr/bin/env python3
"""Verify every git submodule's HEAD matches the SHA pinned in vendor.json."""
import json, subprocess, sys
from pathlib import Path

root = Path(__file__).parent.parent
manifest = json.loads((root / "src/third_party/vendor.json").read_text())

errors = []
for v in manifest["vendors"]:
    sub_path = root / v["submodule_path"]
    if not (sub_path / ".git").exists():
        errors.append(f"  ✗ {v['name']}: submodule not initialised at {sub_path}")
        continue
    actual = subprocess.check_output(
        ["git", "-C", sub_path, "rev-parse", "HEAD"],
        text=True,
    ).strip()
    expected = v["commit_sha"]
    if actual != expected:
        errors.append(
            f"  ✗ {v['name']}: submodule HEAD={actual[:12]}  manifest={expected[:12]}"
        )
    else:
        print(f"  ✓ {v['name']}: {actual[:12]} matches manifest")

if errors:
    print("\nVendor drift detected:")
    for e in errors:
        print(e)
    sys.exit(1)
```

Wire into CI: `ci.yml` and `interop.yml` add a `vendor-check` step that
runs `python tools/check_vendor.py` after checkout.

### Phase 3 — submodule switch (1 day per vendor)

For each vendor in turn:

1. `git mv src/third_party/<name>/src /tmp/<name>-src.bak` (or delete
   after backup if the tree is recoverable).
2. `git submodule add --branch <branch> <url> src/third_party/<name>/src`
3. `cd src/third_party/<name>/src && git checkout <pinned-sha>`
4. Verify `cmake --preset debug.msvc` still configures + builds.
5. `git rm --cached -r src/third_party/<name>/src/.gitkeep` (if any).
6. Commit as a single squashed PR per vendor.

**Order by risk**: nlohmann_json (header-only, 3 files) → googletest
(test-only) → libjuice → libopus → libsrtp → mbedtls (historical) →
wolfssl (production DTLS — most cautious) → webrtc_audio_processing
(meson-built, most disruptive).

Each PR must:

- Show the source-tree size delta (before/after `du -sh`).
- Pass `cmake --preset debug.msvc` + `ctest --preset tests.msvc` on
  Windows (the primary dev env).
- Pass `cmake --preset debug` + `ctest --preset tests` on Linux
  (runner `linux-gcc`).
- Update `vendor.json` and `.gitmodules` in the same commit.

### Phase 4 — CI integration (1 day)

Add `tools/check_vendor.py` as a required CI step. Two policies:

- **Pre-1.0 (now)**: `continue-on-error: true`, only emit a GitHub
  Step Summary. Lets drift surface without blocking unrelated PRs.
- **Post-1.0**: `continue-on-error: false`, drift = red CI = blocked
  merge.

Wire into `ci.yml` `windows` job (after `actions/checkout`):

```yaml
- name: Verify vendor manifests
  if: hashFiles('src/third_party/vendor.json') != ''
  run: python tools/check_vendor.py
```

Also add the same step to `linux-gcc`, `linux-aarch64`, `macos-clang`.

### Phase 5 — remove legacy `SOURCE_VERSIONS` (1 day)

`SOURCE_VERSIONS` was designed for the "git clone --depth 1" workflow
that preceded submodule migration. Its data now lives in `vendor.json`
(SHA + upstream_version per vendor) and `.gitmodules` (URL + branch).
The file can be deleted once `vendor.json` is the authoritative source.

## 4. Open decisions

| # | Decision | Default | Notes |
|---|----------|---------|-------|
| D1 | Where to put the WebRTC APM submodule? | `src/third_party/webrtc_audio_processing/src` | Matches current dir layout; CMake wrapper reads `build_meson/` (gitignored). |
| D2 | Keep `nlohmann_json` vendored or as submodule? | **Submodule, optional** | Header-only, 0.9 MB — submodule is cheap and matches the others. |
| D3 | What to do with the historical mbedtls tree? | **Archive, keep submodule** | Full removal is a separate PR per CHANGELOG `[0.9.0-rc1]`. |
| D4 | Allow `branch = master` pins (no tagged release)? | **Yes**, with `commit_sha` as ground truth | libjuice and libsrtp don't tag frequently; pinning the HEAD SHA is sufficient. |
| D5 | WebRTC APM submodule — clone `master` or a stable commit? | **master** | PulseAudio fork moves slowly; no recent stable tag. |

## 5. Risks

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| Submodule SHA pinning breaks offline builds | High | Blocker | Keep CI's checkout step using `submodules: recursive` (already in `ci.yml`) + a `git submodule update --init` fallback; document `tools/fetch_vendor.sh` for air-gapped environments. |
| Submodule URL becomes unavailable (e.g. PulseAudio mirror outage) | Low | High | Document the `--mirror google` fallback in `tools/fetch_webrtc_apm.py`; keep cached tarballs in `build/_deps-cache/`. |
| `git submodule update --remote` accidentally bumps wolfssl and breaks DTLS | Medium | Blocker | Lock to tag (`v5.9.2`) not branch; `tools/check_vendor.py` is the guardrail. |
| CMake `add_subdirectory` path semantics differ between submodule + vendored layouts | Medium | Medium | wolfSSL's CMakeLists already uses `${_src_dir}`; verify no other vendor hard-codes parent paths during Phase 3 PRs. |
| Migration PRs leak source-tree deltas (huge diffs) | High | Medium | Squash each vendor migration into one commit; use `git rm -r` for the old tree; prefer `git filter-repo` afterwards to flatten history. |

## 6. Commit / PR sequencing

1. **PR A** — `chore(vendor): add vendor.json + .gitmodules schema` (this PR, schema only).
2. **PR B** — `chore(vendor): fill missing SHAs in vendor.json` (fetch tags, no source changes).
3. **PRs C–J** — One per vendor, in the order in §3 Phase 3.
4. **PR K** — `ci: add tools/check_vendor.py gate`.
5. **PR L** — `chore(vendor): delete src/third_party/SOURCE_VERSIONS`.

Estimated total: 8–10 days of focused work across ~10 PRs.

## 7. Reference

- `src/third_party/SOURCE_VERSIONS` — current authoritative version list (to be retired in PR L).
- `src/third_party/CMakeLists.txt` — `_nimrtc_vendor_add_if_populated()` helper that already does the right thing whether the source is vendored or a submodule.
- `docs/plan/linux-x86_64-support.md` §10 — prior decision D2 (CI not blocking until 5 consecutive greens) carries over to the vendor-check gate.
