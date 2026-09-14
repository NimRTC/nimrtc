# ADR-009: PAL Slice 1 — engine plugin resolver seam

| | |
|---|---|
| Number | 009 |
| Status | **Accepted** |
| Date | 2026-09-14 |
| Phase | P1.1 (between P1 exit and P2 kickoff) |
| Bound to | v0.10.0 |
| Supersedes | — |
| Related | `docs/plan/pal-architecture.md` (full architecture, Phase 2 draft) |

## Context

`src/engine/src/engine.cpp` currently walks `core::PluginRegistry` directly
at three call sites:

| Line | Call |
|---|---|
| ~394 | `reg.get_audio3a(config_.audio3a_name)` |
| ~430 | `reg.get_codec(config_.codec_name)` |
| ~668 | `reg.get_video_codec(config_.video_codec_name)` |

These are already backend-agnostic at the registry level. They look alike
but are not funnelled through a single namespace, so any future change
(ENV override, test stub, build-time selection) requires `engine.cpp`
edits. The full architectural motivation is in
`docs/plan/pal-architecture.md` §1.

PAL (Plugin Adaptation Layer) is the seam that gives the engine a single,
thin, backend-agnostic lookup surface so that:

(a) new backends are added without editing `engine.cpp`, and
(b) the migration is incremental, with each PR smaller than ~200 lines.

The architecture document specifies three slices:

| Slice | LOC | Scope |
|---|---|---|
| 1 | ~50 | inline forwarders `pal::resolve_audio3a` / `resolve_codec` / `resolve_video_codec` |
| 2 | ~80 | `kDefaultRegistrars[]` table replaces the inline `register_all_default_plugins()` body |
| 3 | ~30 | `NIMRTC_PLUGIN_ID(name)` compile-time id validation |

This ADR binds **only Slice 1** to v0.10.0. Slices 2 and 3 land on the
v0.10.x patch track and are tracked separately (no ADR required at this
stage — Slice 1 is the structural change with the lowest risk).

## Decision

1. **Add** `src/engine/include/nimrtc/engine/pal/engine_plugin_resolver.hpp`
   with three inline forwarders:

   ```cpp
   namespace nimrtc::engine::pal {
   const plugins::IAudio3AFactory*     resolve_audio3a(std::string_view id) noexcept;
   const plugins::ICodecFactory*       resolve_codec   (std::string_view id) noexcept;
   const plugins::IVideoCodecFactory*  resolve_video_codec(std::string_view id) noexcept;
   }  // namespace nimrtc::engine::pal
   ```

   Implementation: header-only `inline` wrappers forwarding to
   `core::PluginRegistry::get_<category>(id)`. **Zero runtime overhead.**

2. **Update** the three call sites in `engine.cpp` to call `pal::*` instead
   of `reg.get_*()`. No change to public API (`NimRTCEngine`).

3. **Verify** Slice 1 against the regression gate
   `tests/test_engine_plugin_loading.cpp` (7 currently-passing subtests on
   Windows MSVC + Linux x86_64).

## Consequences

### Positive

- Engine source no longer names `core::PluginRegistry::get_*` directly;
  future backend additions do not require `engine.cpp` edits.
- Slice 1 is a literal inline forward — the regression surface is the
  three call sites themselves, nothing else.
- Sets up Slice 2 (`kDefaultRegistrars[]` table) and Slice 3
  (`NIMRTC_PLUGIN_ID()` macro) for v0.10.x patches with no further
  architectural decisions required.

### Negative / risks

- The three new inline forwarders add one header per PAL consumer; if
  consumers start depending on PAL transitively, future PAL API changes
  become harder. Mitigated by keeping PAL header-only and
  backward-compatible-by-construction (each forwarder takes/returns
  exactly what `reg.get_*()` did).

### Neutral

- No public API change. `git diff v0.9.2..v0.10.0 -- src/engine/include/nimrtc/engine/engine.hpp`
  must show zero lines.
- No new dependencies.
- No behaviour drift by construction.

## Verification

| Check | Where | Required |
|---|---|---|
| `tests/test_engine_plugin_loading` 7 subtests pass | Windows MSVC + Linux x86_64 | green |
| `engine.cpp` no longer calls `core::PluginRegistry::get_*` directly | code review | yes |
| Public `NimRTCEngine` API byte-identical to v0.9.2 | `git diff -- 'src/engine/include/**'` | zero lines |
| `e2e` Case D (Chrome interop) still PASS | `python tools/run_e2e_acceptance.py` | green |

## Out of scope (this ADR)

- Slice 2 (`kDefaultRegistrars[]`) and Slice 3 (`NIMRTC_PLUGIN_ID`) — v0.10.x.
- Transport / ICE plugin categories (`IICETransportFactory`) — explicitly
  out per `pal-architecture.md` §3.5.
- Multiple simultaneous backends / merge strategies — Phase 3, deferred.
- ENV-overridden lookups — Phase 3, deferred.
- Inspector / dependency resolver — not built today, deferred.

## References

- `docs/plan/pal-architecture.md` (full Phase 2 design)
- `docs/plan/v0.10-plan.md` (milestone that ships this ADR)
- ADR-001 (plugin system, parent architecture)
