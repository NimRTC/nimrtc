# ADR-020: `EngineConfig` string ownership — the engine owns its strings, and the `std::string_view` migration is complete

| | |
|---|---|
| Number | 020 |
| Status | **Accepted** |
| Date | 2026-11-13 |
| Phase | Phase 5 (v1.2.0) — documentation & ADR sync |
| Bound to | v1.2.0 |
| Supersedes | — |
| Related | `docs/plan/architecture-remediation-plan.md` §7.1 / §7.2 / §7.3; `docs/api/engine_config.md`; `docs/troubleshooting.md` §2.3; `src/engine/include/nimrtc/engine/engine.hpp`; `src/engine/src/engine.cpp`; `docs/adr/ADR-015-api-stability-policy.md` |

## Context

`EngineConfig` is the single config bag a host passes to
`NimRTCEngine`. It carries roughly forty string-valued fields: plugin
ids, hostnames, addresses, format strings, and policy selectors.

The remediation plan (§7.3) records that Phase 1 changed these fields
from `std::string_view` to `std::string`, and that the change is
**ABI-breaking** — a `std::string_view` member and a `std::string`
member have different sizes and different alignment, so the struct's
layout changes and any host that was compiled against the old header
passes a differently-shaped object to the new constructor.

The migration is **done in the code**: every string-valued field in
`src/engine/include/nimrtc/engine/engine.hpp` is now a `std::string`.
But three things were left undone, and together they make the
published documentation actively wrong:

1. **`docs/api/engine_config.md` still describes the pre-migration
   shape.** Its §2 opens with "Each field is a `std::string_view` (or
   `std::string` for fields that may be left empty to mean
   "default")" — a distinction that no longer exists, since *every*
   field is a `std::string`. Its §2.7 reproduces the old field
   declarations verbatim:

   ```cpp
   std::string_view video_source_name     = "memory";
   std::string_view video_sink_name       = "headless";
   std::string_view video_receiver_name   = "reference";
   std::string_view video_sender_name     = "reference";
   ```

   Copying those into a host produces code that does not compile
   against the current header.

2. **`docs/troubleshooting.md` §2.3 teaches the workaround for the old
   shape.** Its title is "`use of deleted function` when constructing
   EngineConfig", and its body says "`EngineConfig` uses
   `std::string_view` for plugin ids. Assigning from a `const char*` is
   fine; from an `std::string`, you must take `.c_str()` or rely on the
   `std::string_view` constructor". That advice is now obsolete — and
   worse, the `.c_str()` workaround it teaches is **the exact hazard
   the migration removed**. A `std::string_view` field pointed at a
   `const char*` that the host later frees is a dangling view; a
   `std::string` field copies.

3. **The ownership semantics were never written down.** The code has a
   definite and good answer — the engine takes the config **by value**
   and moves it:

   ```cpp
   NimRTCEngine::NimRTCEngine(EngineConfig config)
       : impl_(new Impl()), config_(std::move(config)) { ... }
   ```

   and the constructor is documented as "The caller may destroy /
   mutate `config` after this returns." But that is a comment in one
   header, and the *return* direction — `engine.config()` hands back
   a `const EngineConfig&` that aliases the engine's own copy, and
   `transport_strategy_session()` hands back `std::string_view`s that
   alias the engine's internal `std::string`s — is not documented
   anywhere.

### Why the `string_view` shape was wrong

The original motivation for `std::string_view` fields was to avoid a
copy per field at config construction. That motivation does not
survive contact with the actual usage pattern:

- `EngineConfig` is constructed **once per engine**, and engines are
  constructed **once per session**. Forty small-string copies at
  session setup are not a measurable cost next to opening a DTLS
  handshake or gathering ICE candidates.
- The `std::string_view` shape created a **lifetime hazard at every
  assignment site**. A host that builds a config from a `std::string`
  it owns must either copy anyway (`.c_str()` plus a view that the
  engine reads later) or keep the original alive for the engine's
  lifetime. Neither is obvious, and both are easy to get wrong.
- The engine *copied the config anyway*. `config_(std::move(config))`
  moves the struct, but the struct's members are views into storage
  the engine does **not** own — so the "move" was a move of dangling
  views, and the engine was, in effect, already owning copies of
  everything it needed anyway.

The `std::string` shape removes the hazard by construction: the engine
owns a real copy of every string, and the host's original can go away
immediately.

## Decision

### Decision 1 — The migration is complete and is the supported shape

Every string-valued field of `EngineConfig` is a `std::string`. There
are no `std::string_view` members left in the struct, and there is no
"empty means default" distinction based on the *type* — empty is a
**value**, and every field's default is a value (usually a non-empty
id, sometimes `""`).

This is the v1.2.0 ABI break recorded in the remediation plan §7.3,
and per ADR-015 it ships as a MINOR version bump with a migration
note. The migration note is the content of `docs/api/engine_config.md`
§2 as rewritten by this ADR.

### Decision 2 — The engine owns its copy of the config

`NimRTCEngine(EngineConfig config)` takes the config **by value** and
moves it into the engine's `config_` member. The contract:

- The caller may destroy, mutate, or let the original go out of scope
  immediately after the constructor returns. The engine's copy is
  independent.
- The engine's `config_` member is the **only** copy the engine keeps.
  It is never aliased to caller-owned storage.

This is already the implemented behaviour (`std::move(config)` in
`src/engine/src/engine.cpp`), and this ADR makes it the documented
contract.

### Decision 3 — `engine.config()` returns a borrowed view, valid for the engine's lifetime

`const EngineConfig& NimRTCEngine::config() const` returns a reference
to the engine's own `config_` member. The contract:

- The reference is valid **only while the engine is alive**.
- The reference is **const**. A host cannot mutate the engine's config
  through it.
- The referenced strings are **owned by the engine**, not by the
  caller. A `std::string_view` obtained from them (`cfg.dtls_name` →
  view) is valid as long as the engine lives.

A host that needs to keep the config past the engine's lifetime must
**copy** it:

```cpp
nimrtc::engine::EngineConfig snapshot = engine.config();   // copy — outlives the engine
```

### Decision 4 — Output views alias engine-internal storage

Two accessors return `std::string_view`s that alias the engine's
internal `std::string`s:

```cpp
struct NIMRTC_API_STABLE TransportSessionView {
    std::string_view media_factory_id;
    std::string_view control_factory_id;
};
NIMRTC_API_STABLE TransportSessionView transport_strategy_session() const noexcept;
```

and the implementation is:

```cpp
return {std::string_view{impl_->resolved_media_factory_id},
        std::string_view{impl_->resolved_control_factory_id}};
```

The contract:

- The views are valid **only while the engine is alive**, and only
  until the engine re-resolves the transport strategy (which happens
  at `open()` and on a transport-stack change). A view obtained
  before such a change **must not** be read after it.
- The views are **borrowed**, not owned. A host that needs to keep
  them must copy to `std::string`.

This is the same borrowed-view pattern used throughout the codebase
for output parameters (`add_turn_server(std::string_view host, ...)`,
`create_data_channel(std::string_view label, ...)`) and is consistent
with it: `std::string_view` in an **input** position means "the caller
owns this, I will read it during the call"; `std::string_view` in an
**output** position means "I own this, you may read it until I change
it".

The asymmetry is deliberate and is the reason the `EngineConfig`
migration was necessary: **input** positions can safely use
`std::string_view` (the lifetime is bounded by the call), while a
**stored** field cannot (its lifetime is unbounded by the call).

### Decision 5 — Documentation must describe the shape that exists

`docs/api/engine_config.md` is corrected to describe `std::string`
throughout, with the migration note that ADR-015's MINOR-bump policy
requires. `docs/troubleshooting.md` §2.3 is rewritten: the
"use of deleted function" entry becomes a migration entry that
explains what changed, why, and how to update a host.

The rule this establishes for the future: **a doc that shows a
declaration of a public struct is a specification of that struct's
layout.** When the struct changes, the doc changes in the same commit.
The remediation plan's §7.1 task list was created for exactly this
reason and had simply not been executed for this file.

## Consequences

### Positive

- **The dangling-view hazard is gone.** A host cannot create one by
  storing a `const char*` in a config field, because the field copies.
- **`engine.config()` is safe to read for the engine's lifetime**, and
  the rule for outliving it is one obvious copy.
- **The docs stop teaching a workaround for a bug that no longer
  exists**, and start documenting the ownership rule that makes the
  current design safe.
- **The ABI break is properly disclosed**, in the place a migrating
  host will look (`docs/api/engine_config.md`) rather than only in the
  remediation plan's internal §7.3.

### Negative / costs

- **A copy per field at config construction.** Forty `std::string`
  copies at session setup. Not measurable against session setup cost.
- **A larger `EngineConfig`.** `std::string` is 32 bytes on the common
  implementations; `std::string_view` is 16. `EngineConfig` grows by
  roughly 640 bytes. It is a stack-allocated temporary in the common
  case, and the cost is a memcpy the CPU does while the CPU is idle
  waiting for a socket.
- **Existing hosts must recompile.** Inherent to the ABI break; the
  migration note is the mitigation.

### Neutral

- No change to the engine's *resolution* semantics. Which plugin a
  field selects, and what happens when an id is unknown, are governed
  by ADR-019 and by the failure-mode table in
  `docs/plugin_author_guide.md` §4.1 — not by this ADR.
- `EngineConfig`'s non-string fields (the tuning structs, the numeric
  knobs) are unaffected.

## Alternatives considered

### A. Revert to `std::string_view`

Rejected. It re-introduces the dangling-view hazard at every
assignment site, and the copy cost it saves is unmeasurable. The
migration is already merged and tested; reverting is strictly more
work than documenting it.

### B. `EngineConfig` holds `std::string_view` but the engine deep-copies

Rejected. This is the worst of both: the host still faces the
assignment-site hazard (it must keep its own strings alive until
`open()`), *and* the engine pays the copy it was trying to avoid. It
buys nothing.

### C. Make `EngineConfig` immutable and shared (`shared_ptr<const EngineConfig>`)

Rejected for now. It would solve the ownership question by removing
mutation, but `EngineConfig` is currently an aggregate the host
mutates field-by-field before constructing the engine
(`cfg.dtls_name = "gmssl";`). A shared, immutable value would require
a builder or a copy-on-write wrapper, which is a much larger API
change than the problem warrants — and would break the aggregate
initialisation style hosts use.

### D. Keep `std::string_view` for fields that are "just ids"

Rejected. The id fields are exactly the ones most likely to be
assigned from a computed `std::string` (`cfg.dtls_name = backend_name;`
where `backend_name` is a runtime lookup), so they are the fields most
in need of copying.

## References

- `docs/plan/architecture-remediation-plan.md` §7.1 (doc list), §7.2
  (this ADR), §7.3 (version strategy).
- `docs/adr/ADR-015-api-stability-policy.md` — the MINOR-bump policy
  this ADR's migration note implements.
- `docs/adr/ADR-019-plugin-id-naming-contract.md` — the companion
  decision on how a plugin id is spelled.
- `docs/api/engine_config.md` — the field reference this ADR corrects.
- `docs/troubleshooting.md` §2.3 — the migration entry this ADR
  rewrites.
- `src/engine/include/nimrtc/engine/engine.hpp` — `EngineConfig`,
  `config()`, `TransportSessionView`.
- `src/engine/src/engine.cpp` — the by-value, move-in constructor.
