# Plugin Adaptation Layer (PAL) — Architecture

| | |
|---|---|
| Version | v1.0 (draft) |
| Date    | 2026-09-10 |
| Status  | Phase 2 — design agreed, first slice pending MSVC/Win CI baseline |
| Scope   | Engine ↔ Plugin-registry seam; backend substitution; cross-platform parity |

> PAL is a **seam**, not a feature. Its purpose is to give engine source code a single,
> thin, backend-agnostic lookup surface so that:
> (a) new backends are added without editing `engine.cpp`, and
> (b) the migration is incremental, with each PR smaller than ~200 lines.

---

## 1. Problem statement

Today `src/engine/src/engine.cpp` already goes through `core::PluginRegistry` for plugin
look-up, which is good. The remaining hard couplings live in **two places**:

### 1.1 Hard-coded registration list

`src/core/include/nimrtc/core/registry.hpp` lines ~594–614:

```cpp
inline void register_all_default_plugins() noexcept {
    nimrtc::ice::register_default_plugins();
    nimrtc::rtp::register_default_plugins();
    nimrtc::sdp::register_default_plugins();
    nimrtc::jb::register_default_plugins();
    nimrtc::audio3a::register_default_plugins();
#ifdef NIMRTC_HAS_OPUS
    nimrtc::opus::register_default_plugins();
#endif
#ifdef NIMRTC_HAS_H264
    nimrtc::h264::register_default_plugins();
#endif
#ifdef NIMRTC_HAS_VIDEO_SOURCE
    nimrtc::video_source::register_default_plugins();
#endif
#ifdef NIMRTC_HAS_VIDEO_PIPELINE
    nimrtc::video_pipeline::register_default_plugins();
#endif
#ifdef NIMRTC_HAS_VIDEO_SINK
    nimrtc::video_sink::register_default_plugins();
#endif
    nimrtc::bwe::register_default_plugins();
    nimrtc::sched::register_default_plugins();
}
```

Every new module forces an edit here. Today the list is ~13 hard-coded names; six are
behind `NIMRTC_HAS_*` compile-time guards, the remaining seven (`ice`, `rtp`, `sdp`,
`jb`, `audio3a`, `bwe`, `sched`) are unconditional and will **link-fail** if the
corresponding module library is not linked into the consumer binary. (Pre-existing latent
bug: this is why `bwe` + `sched` must always be linked today — they have no `#ifdef`
guard.)

### 1.2 Engine-internal call sites that walk the registry directly

`src/engine/src/engine.cpp`:

| Line | Call | Note |
|---|---|---|
| 394 | `reg.get_audio3a(config_.audio3a_name)` | direct registry read |
| 430 | `reg.get_codec(config_.codec_name)` | direct registry read |
| 668 | `reg.get_video_codec(config_.video_codec_name)` | direct registry read |

These are **already backend-agnostic** at the registry level. But each one is an
unstructured `core::PluginRegistry::get_<category>(name)` lookup. They look alike but
are not funneled through a single namespace, so a future change (e.g. ENV override,
test stub, build-time selection) would require `engine.cpp` edits.

### 1.3 Same plugin-id strings are duplicated

`config_.audio3a_name`, `config_.codec_name`, `config_.video_codec_name` defaults live
in `config.hpp`. The on-wire id strings (`"opus"`, `"h264"`, `"webrtc"`,
`"webrtc_apm"`) live in module source files (`audio3a.cpp`, `opus.cpp`,
`h264/codec_plugin.cpp`). The mapping is implicit — a mismatch is only caught at
runtime when the registry returns `nullptr`.

---

## 2. Goal

Define the engine ↔ backend contract so that:

1. **No public API breakage.** `src/engine/include/nimrtc/engine/engine.hpp`
   keeps the current `NimRTCEngine` class methods, signatures, and return codes
   byte-for-byte.
2. **No behavioural drift.** The 7 currently-passing sub-tests of
   `EnginePluginLoading` (in `tests/test_engine_plugin_loading.cpp`) keep passing.
   The 1 pre-existing failing test (`engine_send_audio_path_is_reachable`) is
   out of scope and stays failing.
3. **Windows + Linux build parity.** Every header in the PAL surface area compiles
   on MSVC. No POSIX-only code in shared headers.
4. **Engine.cpp no longer hard-codes** module names for plugin lookup. New backends
   do not require editing `engine.cpp`.

---

## 3. Design

### 3.1 Surface area (minimum viable PAL)

```cpp
// src/engine/include/nimrtc/engine/pal/engine_plugin_resolver.hpp
namespace nimrtc::engine::pal {

const plugins::IAudio3AFactory* resolve_audio3a(std::string_view id) noexcept;
const plugins::ICodecFactory*    resolve_codec   (std::string_view id) noexcept;
const plugins::IVideoCodecFactory* resolve_video_codec(std::string_view id) noexcept;

}  // namespace nimrtc::engine::pal
```

Implementation: header-only `inline` wrappers that forward to
`core::PluginRegistry::get_<category>(id)`. **Zero runtime overhead** vs. current.

### 3.2 Factory-id naming convention (backward-compatible)

Existing ids must remain identical: `"opus"`, `"h264"`, `"webrtc"`, `"webrtc_apm"`.
PAL does **not** introduce new id strings. The id remains the registry key.

### 3.3 Self-registration

Replace the hard-coded `register_all_default_plugins()` body with a list
**maintained in `src/core/src/pal_default_registrars.cpp`** (`core` already knows
about every module):

```cpp
// src/core/src/pal_default_registrars.cpp  (sketch)
namespace nimrtc::core::detail {
using RegistrarFn = void(*)() noexcept;
constexpr RegistrarFn kDefaultRegistrars[] = {
    &nimrtc::ice::register_default_plugins,
    &nimrtc::rtp::register_default_plugins,
    &nimrtc::sdp::register_default_plugins,
    &nimrtc::jb::register_default_plugins,
    &nimrtc::audio3a::register_default_plugins,
#ifdef NIMRTC_HAS_OPUS
    &nimrtc::opus::register_default_plugins,
#endif
    ...
};
}
```

The list still lives in one place, but it is now an **explicit, iterable,
testable array** rather than an inline statement list. A test can iterate
`kDefaultRegistrars` after a `register_all_default_plugins()` call and assert
no factory id duplicates itself.

### 3.4 Lifetime / ownership

- Factories continue to be plain `const T*` pointers, owned by the static
  module-scope `WebRtcPluginFactory` / etc. instances.
- Registry owns the `id → factory*` map; lifetime is process-lifetime.
- PAL resolves lookups — owns nothing. It is stateless beyond the static
  inline forwarders.

### 3.5 What PAL does NOT do (out of scope here)

- **Transport / ICE source/sink plugin categories.** `nimrtc::ice::register_default_plugins`
  registers `IICETransportFactory*`, which is closely tied to the engine's
  ownership of `ice_t_`. PAL does not touch this in Phase 2.
- **Multiple simultaneous backends / merge strategies.** "Configure two Opus
  backends and pick at runtime." Deferred to Phase 3.
- **ENV-overridden lookups** (e.g. `NIMRTC_OPUS_FACTORY=stub`). Deferred to Phase 3.
- **Inspector / dependency resolver.** "Does the h264 factory require opus?"
  Not built today; deferred.

---

## 4. Migration path (3 PR-sized slices)

### Slice 1 — PAL seam (this document, ~50 LOC code)

- Add `src/engine/include/nimrtc/engine/pal/engine_plugin_resolver.hpp` with
  three inline forwarders.
- Update `engine.cpp` line 394, 430, 668 to call `pal::resolve_audio3a()` etc.
  instead of `reg.get_*()`. No header changes to public API.
- Verify: `tests/test_engine_plugin_loading.cpp` is the regression gate.
- Risk: zero — `pal::*` literally calls the same registry function.

### Slice 2 — Self-registration list (~80 LOC code)

- Add `src/core/src/pal_default_registrars.cpp` with the
  `kDefaultRegistrars[]` table.
- Rewrite `core::register_all_default_plugins()` to iterate the table.
- Verify: existing tests; idempotency test asserts no double-registration.
- Risk: low — single inline call site, all 13 module libraries still linked.

### Slice 3 — Id-tagging macros for compile-time validation (~30 LOC)

- Add `NIMRTC_PLUGIN_ID(name)` literal-string macro in plugins/
- Apply to the 13 module-side default factories (`"opus"`, `"h264"`, …).
- Verify: a static_assert at registry insertion that the id literal is
  non-empty.
- Risk: very low; purely additive.

Each slice ≤ 200 LOC diff.

---

## 5. Out of scope for this slice

- Code changes (see §4 Slice 1, blocked on a Win/MSVC baseline build).
- Renaming any `plugins::*` interface.
- Adding new error codes.
- Changing `NIMRTC_HAS_*` compile-time guards.
- Touching `tests/` files.
- Linux-side changes — PAL is platform-neutral by design.

---

## 6. Verification

| Check | Where | Required |
|---|---|---|
| `tests/test_engine_plugin_loading` — 7 currently-passing subtests | Windows MSVC build via Win dev machine | green |
| Linux WSL2: `cmake --build --preset debug && ctest --preset tests` | WSL2 (already 100% / 98% baseline) | no regression vs. baseline |
| `engine.cpp` no longer names `core::PluginRegistry::get_*` directly | code review | yes |
| Public `NimRTCEngine` API unchanged | `git diff` of `engine.hpp` | zero lines |

The Linux ctest baseline is already established (98% pass, 315/321). The Windows
re-validation must run on the Win dev machine that has MSVC installed — the
present sandbox does not have `cl.exe` so the **command output** for that step
will come from the dev environment, not from the sandbox.

