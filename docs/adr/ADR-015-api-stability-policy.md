# ADR-015: NimRTC API Stability Policy

| | |
|---|---|
| Number | 015 |
| Status | **Accepted** |
| Date | 2026-10-13 |
| Phase | P2 (v1.0.0) |
| Bound to | v1.0.0 |
| Supersedes | — |
| Related | `docs/plan/v1.0-plan.md` (parent plan), `docs/adr/ADR-001-plugin-system.md` (plugin architecture), `docs/adr/ADR-008-pcm-tap.md` (first `@deprecated` comment precedent), `src/core/include/nimrtc/core/api_version.hpp` (the `NIMRTC_API_VERSION` framework) |

## Context

Every `v0.x` release note has repeated the same caveat:

> "API still experimental / not for production through v1.0.0"

This banner has been necessary — the public header surface was growing
without a documented commitment to stability. The repeated caveat was a
placeholder: *v1.0.0 will be the release that drops it*.

To drop the banner honestly, v1.0.0 must formalise four things that
have been implicit but unenforceable:

1. **A version identifier** — callers can inspect the header version at
   compile time and assert on the expected value. Without this, a
   downstream pinning to `v1.0.0` is not meaningful.
2. **A classification scheme** — every public symbol must be labelled
   STABLE, UNSTABLE, or DEPRECATED. Without this, "stable" is
   a marketing claim with no engineering substance.
3. **A deprecation policy** — the process by which a symbol transitions
   from STABLE → DEPRECATED → removed. Without this, a future
   v1.1.0 can silently break a v1.0.0 consumer.
4. **An ABI commitment** — what a consumer built against v1.0.0 can
   expect when NimRTC ships v1.0.1, v1.0.2, … patch releases.

These four components are implemented by the companion PR
**v1.0.0-alpha.1 PR #1** (`src/core/include/nimrtc/core/api_version.hpp`),
which introduces the `NIMRTC_API_VERSION`,
`NIMRTC_API_STABLE`, `NIMRTC_API_UNSTABLE`, and
`NIMRTC_API_DEPRECATED` macro family. This ADR is the policy that
governs how those macros are used and what semantics they carry.

The first precedent for a `@deprecated` annotation in the codebase is
ADR-008's PCM tap interface (marked `@deprecated` in v0.11.0 with
removal planned for v1.2.0, per `v0.11-plan.md` §10). ADR-015
generalises that ad-hoc pattern into a formal policy.

## Decision

**NimRTC adopts the following four-component API stability policy for
all public symbols under `src/**/include/nimrtc/**`.**

### Decision 1 — Public-header version macro

Every public header (or a single central header included by all
consumers) exposes version integers and a pre-release tag:

```cpp
#define NIMRTC_API_VERSION_MAJOR  1
#define NIMRTC_API_VERSION_MINOR  0
#define NIMRTC_API_VERSION_PATCH  0
#define NIMRTC_API_VERSION_STRING "1.0.0"
#define NIMRTC_API_VERSION_PRE_RELEASE ""   // "alpha.1", "beta.1", "rc.1", or "" for final
```

- **`MAJOR`** bumps for breaking changes (see Decision 3).
- **`MINOR`** bumps for additive changes (new public symbols, new
  optional parameters with defaults).
- **`PATCH`** bumps for bug-fix-only changes (no public-API change).
- **`NIMRTC_API_VERSION_PRE_RELEASE`** carries the semver pre-release
  suffix (`"alpha.1"`, `"beta.1"`, `"rc.1"`, `""` for final).  The
  v1.0.0 cycle uses `MAJOR=1`; pre-release suffixes follow
  `docs/plan/v1.0-plan.md` §12 progression.

Consumers can assert at compile time:

```cpp
static_assert(NIMRTC_API_VERSION_MAJOR == 1 && NIMRTC_API_VERSION_MINOR >= 0);
```

### Decision 2 — Stability classification

Every public symbol in every `nimrtc/*` header is annotated with one
of three classification markers:

| Marker | Meaning | Compile-time effect |
|---|---|---|
| `NIMRTC_API_STABLE` | Public, ABI-stable across MINOR/PATCH of the same MAJOR. Removing a STABLE symbol requires a MAJOR bump + deprecation in the prior MAJOR for at least one MINOR cycle. | None (visual documentation) |
| `NIMRTC_API_UNSTABLE` | Public, may change in any MINOR bump. UNSTABLE symbols must be promoted to STABLE or DEPRECATED within at most 2 MINOR cycles of their introduction. | None (visual documentation) |
| `NIMRTC_API_DEPRECATED(msg)` | Public, scheduled for removal. Emits a compile-time warning on caller use (`-Wdeprecated-declarations` on GCC/Clang, `C4996` on MSVC). | **Warning on use** |

All three markers are visual documentation in default mode (no
compile-time effect for `STABLE` / `UNSTABLE`). Only
`NIMRTC_API_DEPRECATED(msg)` emits a warning. This keeps the macro
set purely additive — v0.x callers who include the header without
opting into the new annotations do not pick up new warnings.

Example usage in a public header:

```cpp
namespace nimrtc {
NIMRTC_API_STABLE
class NIMRTC_API IAudio3A : public IPlugin {
    NIMRTC_API_STABLE
    virtual void set_pre_process_tap(PcmTapCallback tap) noexcept = 0;

    NIMRTC_API_DEPRECATED("Use set_post_process_tap() instead; removed in v2.0.0")
    virtual void legacy_tap(PcmTapCallback tap) noexcept = 0;
};
} // namespace nimrtc
```

### Decision 3 — Deprecation policy

When a public symbol is to be removed or changed in a way that is not
backwards-compatible, the following three-step process applies:

**Step 1 — Mark**: In the commit that introduces the deprecation, tag the
symbol `NIMRTC_API_DEPRECATED(msg)` and document the planned removal
version in `CHANGELOG.md` under `[Unreleased]` (or the current dev
cycle).  The `msg` argument should name the replacement and the target
removal version.

**Step 2 — Coexist**: The deprecated symbol remains in the public
surface for at least one MINOR release cycle (e.g. v1.0.0 deprecates
X → v1.1.0 ships with X still present but emitting a warning).

**Step 3 — Remove**: Removal happens at:
- A **MAJOR bump** for any symbol declared `NIMRTC_API_STABLE`, OR
- Any **MINOR bump** after the required one-MINOR deprecation window
  for symbols that were never declared `NIMRTC_API_STABLE`.

Example timeline for a STABLE symbol:

```
v1.0.0  X() is STABLE
v1.1.0  X() marked NIMRTC_API_DEPRECATED("Use Y() instead; removed in v2.0.0")
v1.2.0  X() still present, warning on use
v2.0.0  X() removed (MAJOR bump)
```

Example timeline for a symbol that was always UNSTABLE:

```
v1.0.0  Z() is UNSTABLE
v1.1.0  Z() marked NIMRTC_API_DEPRECATED("Use W() instead; removed in v1.2.0")
v1.2.0  Z() removed (MINOR bump; Z() was never STABLE)
```

### Decision 4 — ABI commitment

Within the **v1.0.x line** (MAJOR=1, MINOR=0, PATCH varies):
ABI is committed. A consumer built against v1.0.0 continues to
rebuild and link against v1.0.x patch releases without source changes.

Specific rules:

| Scenario | ABI committed? | Note |
|---|---|---|
| `v1.0.0` → `v1.0.1` (PATCH bump) | **Yes** | Recompile against new headers → working binary. Same `.lib` / `.a` may not work (depends on compiler version); rebuild always works. |
| `v1.0.x` → `v1.1.0` (MINOR bump) | **No** | Callers must rebuild. API is additive (no STABLE-symbol removal); ABI may change. |
| `v1.x` → `v2.0.0` (MAJOR bump) | **No** | Full break; all bets are off. |

This matches semver.org's interpretation of the "v1.0.0 banner":
the first release that makes an explicit ABI commitment. It is not
as strong as a Linux-distro ABI guarantee (ABI-stable across MINOR
bumps), which would be disproportionately costly for a 1-person
plus AI maintainer project. It is sufficient for the use case:
a v1.0.0 consumer can pin to a specific commit or tag and rebuild
confidently for any v1.0.x patch.

## Alternatives Considered

### A — Bump MAJOR for every breaking change

**Semver without MINOR/PATCH granularity**: every non-additive change
is a MAJOR bump. Example: v1.0.0 → v2.0.0 for removing one deprecated
helper function.

**Why not**: too disruptive for the pace of a 1-person + AI project.
Every minor usability improvement or API cleanup would propagate to
every downstream `CMakeLists.txt` or `package.json` pinning the major
version. The MINOR/PATCH tiers exist precisely to absorb
additive/bug-fix changes without forcing MAJOR churn.

### B — ABI-stable across MINOR bumps

**Linux-distro model**: once a symbol is STABLE in v1.0.0, it must
remain ABI-compatible in v1.1.0, v1.2.0, etc., until the next MAJOR.

**Why not**: this forces every new internal implementation detail
(private virtual method additions, new non-virtual helpers) to be
carefully ABI-padded or it becomes a de-facto public commitment. For
a project with one human maintainer, the overhead of auditing every
internal change for ABI impact at each MINOR bump is prohibitive.
The policy chosen here (ABI committed only within v1.0.x patch line)
is a pragmatic middle ground: a pinned consumer is never forced to
rebuild because of a PATCH release, but MINOR and MAJOR bumps are
honestly signalled.

### C — Always-STABLE public API, never deprecate

**"Once public, always public"**: once a symbol is exposed, it can
never be removed or changed, even with a deprecation window.

**Why not**: false. A codebase that never removes or evolves its public
surface accumulates technical debt that eventually makes the API
worse for everyone. ADR-008's PCM tap interface is a concrete example:
the `UsrsctpSocket*` class was marked `@deprecated` in v0.11.0 with
removal planned for v1.2.0. ADR-015 makes this pattern explicit and
enforceable.

## Consequences

### Positive

- **The v1.0.0 "Stable" banner is enforceable**: downstream consumers
  can assert on `NIMRTC_API_VERSION_MAJOR == 1` at compile time and
  know exactly what they are getting.
- **Classification is self-documenting**: `NIMRTC_API_STABLE` /
  `NIMRTC_API_UNSTABLE` are visible in every public header; grep
  confirms the classification without reading this ADR.
- **Deprecation is a first-class signal**: the warning on use (MSVC
  `C4996` / GCC `C4996`-equivalent) makes it easy for consumers to
  find and fix deprecated calls before upgrading.
- **v0.x → v1.0.0 migration is additive-only**: `STABLE` and
  `UNSTABLE` carry no compile-time effect, so v0.x callers who
  include the new header do not receive unexpected warnings.

### Negative / risks

- **Classification sweep risk**: the alpha.1 PR #3 (public header
  classification sweep) may surface 5 or more symbols that are
  "experimental but load-bearing" — used internally but never
  formally evaluated for stability. The v0.13.x sweep is the
  mitigation; any that cannot be stabilised in time move to
  `-internal/` namespace pre-v1.0.0 per `docs/plan/v1.0-plan.md` §5
  and the §3 DoD Gate #2.
- **Enforcement is manual at v1.0.0-alpha.1**: `tests/test_api_version.cpp`
  (alpha.1 PR #1) verifies the macro set is correct, but the
  classification sweep (`tests/test_api_classification.cpp`,
  alpha.1 PR #3) requires human review of every public symbol.
  No automated tool enforces that every new symbol added after v1.0.0
  carries a classification — this is a process discipline, not a
  technical gate.

### Neutral

- Same maintenance overhead as any SemVer-strict project: bumping
  MINOR/PATCH appropriately, marking removals with deprecation.
- Does not add new CI gates beyond the two tests introduced by
  alpha.1 PR #1 and alpha.1 PR #3:
  `tests/test_api_version.cpp` and `tests/test_api_classification.cpp`.

## Verification

| Check | Where | Required |
|---|---|---|
| `tests/test_api_version.cpp` 5/5 PASS | Windows MSVC + Linux x86_64 | green |
| Every public symbol under `src/**/include/nimrtc/**` carries at least one stability annotation | `tests/test_api_classification.cpp` (alpha.1 PR #3) | green; missing annotation → compile error |
| Every symbol marked `NIMRTC_API_DEPRECATED` has a `CHANGELOG.md` entry under `[Unreleased]` or the current dev cycle | code review | yes |
| `NIMRTC_API_VERSION_PRE_RELEASE` is `""` (empty) for any final tag | at tag cut | yes |
| Public-header diff vs v0.12.0: only additive changes | `git diff v0.12.0..HEAD -- 'src/**/include/**'` | only new symbols; zero modified or removed |

## Out of scope (this ADR)

- **v1.0.0-alpha.1 PR #1** (`NIMRTC_API_VERSION` macro framework) —
  the mechanical implementation this ADR defines policy for.
- **v1.0.0-alpha.1 PR #3** (public header classification sweep) — the
  PR that applies the classification markers to all existing public
  symbols.
- **Multiple simultaneous STABLE variants** (e.g. v1.x-stable and
  v2.x-experimental served from the same tree) — deferred to a
  future ADR if the need arises.
- **Per-symbol deprecation windows longer than one MINOR cycle** —
  not needed for v1.0.0; can be revisited if a specific symbol
  warrants it.

## References

- [`docs/plan/v1.0-plan.md`](v1.0-plan.md) — parent plan; §2.1 P0
  API-FREEZE item and §12.3.1 v1.0.0-alpha.1 DoD gates for this ADR.
- [`docs/adr/ADR-001-plugin-system.md`](ADR-001-plugin-system.md) —
  parent architecture; `PluginRegistry` is the first public API to
  carry `NIMRTC_API_STABLE`.
- [`docs/adr/ADR-008-pcm-tap.md`](ADR-008-pcm-tap.md) — the first
  `@deprecated` comment precedent in the codebase; `UsrsctpSocket*`
  marked deprecated in v0.11.0 for removal at v1.2.0.
- [`src/core/include/nimrtc/core/api_version.hpp`](src/core/include/nimrtc/core/api_version.hpp) — the `NIMRTC_API_VERSION` macro
  framework implemented by v1.0.0-alpha.1 PR #1.
- [semver.org/spec/v2.0.0.html](https://semver.org/spec/v2.0.0.html) —
  Semantic Versioning 2.0.0 specification.
