# ADR-019: Plugin ID naming contract — how a plugin id is spelled, validated, and changed

| | |
|---|---|
| Number | 019 |
| Status | **Accepted** |
| Date | 2026-11-13 |
| Phase | Phase 5 (v1.2.0) — documentation & ADR sync |
| Bound to | v1.2.0 |
| Supersedes | — |
| Related | `docs/plan/architecture-remediation-plan.md` §7.1 / §7.2; `docs/adr/ADR-001-plugin-system.md`; `docs/plugin_author_guide.md` §4, §8; `src/core/include/nimrtc/core/registry.hpp`; `src/core/src/pal_default_registrars.cpp` |

## Context

ADR-001 established the string-id seam: a plugin registers itself under a
stable string id, and the host selects it by writing that string into an
`EngineConfig` field. That decision left three things unspecified, and
all three have been felt in practice:

1. **What is the legal spelling of an id?** The registry accepts any
   string. The built-ins use at least four different conventions
   (`"opus"`, `"nimrtc_audio_rx"`, `"mock_tpal4"`, `"web_transport"`),
   with no rule to distinguish them.
2. **Who validates an id, and when?** Nothing does. `TypedRegistry::register_one`
   stores whatever it is handed, including the empty string, with no
   diagnostic. The first place a malformed id is noticed is the engine's
   *lookup* at `open()` time — by which point the host has already
   written a config field that will never match anything.
3. **What is the compatibility promise for an id?** The plugin author
   guide (§8) says "once published, stable for the lifetime of the
   engine" and that renaming "requires a deprecation cycle (run both
   ids under a `register_<family>` shim for at least one minor
   version)". That promise is prose in a guide, not something the
   codebase enforces or tests.

There is also a concrete inconsistency inside the registration macros
themselves. The two families of macro disagree about what the `id`
argument is:

```cpp
// Legacy family — the id is a bare identifier token, stringified by the macro.
#define NIMRTC_REGISTER_DTLS_SESSION(id, factory_ptr) \
    ... Registrar::Category::kDtlsSession, #id, factory_ptr }
    // Usage: NIMRTC_REGISTER_DTLS_SESSION(mybackend, &g_factory);
    //   -> registers under the id "mybackend"

// Audio-seam family — the id is a string literal, passed through as-is.
#define NIMRTC_REGISTER_AUDIO_DECODER(id, factory_ptr) \
    ... Registrar::Category::kAudioDecoder, id, factory_ptr }
    // Usage: NIMRTC_REGISTER_AUDIO_DECODER("opus", &g_opus_decoder_factory);
```

The legacy form *cannot* express an id containing a character that is
not valid in a C++ identifier, and it silently accepts a quoted
literal too — `NIMRTC_REGISTER_DTLS_SESSION("mybackend", &f)` would
register under the id `"mybackend"` **including the quote
characters**, which is a typo that registers successfully and never
resolves. The audio-seam form has the opposite hazard: it accepts a
bare token and fails to compile, which is the better failure, but the
inconsistency is undocumented and the two forms are not
interchangeable.

Meanwhile the built-in default registrars do not use the macros at
all. `src/core/src/pal_default_registrars.cpp` calls
`core::PluginRegistry::register_<family>("<id>", &factory)` directly,
so in-tree ids and third-party ids travel through two different code
paths with two different spelling rules.

## Decision

### Decision 1 — The id is a lowercase ASCII token

A plugin id **must** match:

```
^[a-z][a-z0-9_-]*$
```

That is: starts with a lowercase ASCII letter; continues with lowercase
ASCII letters, digits, underscores and hyphens; no other characters.

**The hyphen is included because two published, user-facing in-tree ids
already use it** — `"webrtc-classic"` and `"raw-udp-arq"`, the
transport-stack factory ids that appear in
`EngineConfig::transport_stack_name`, in the Profile JSON's
`transport.media.stack` / `transport.control.stack` fields, and in
the transport strategy names (`"agent-gateway"`,
`"teleop-ultra-low-latency"`). A grammar that forbade the hyphen
would have invalidated a public config surface that already ships.
The grammar is fitted to the ids that exist, not the other way round.

Everything else about the grammar is as originally proposed:

- **It is a valid C++ identifier except for the hyphen.** The hyphen
  is the one concession to the existing ids, and it has a direct
  consequence for the registration macros: the legacy `#id`
  stringification form cannot express a hyphenated id. That is why
  Decision 5 gives the audio-seam macros a literal-taking form, and
  why a hyphenated third-party id must be registered with the
  literal form or a direct registry call. This is stated explicitly
  rather than left as a trap.
- **It is a valid shell / filename / JSON key token.** Ids appear in
  log lines, in test names, and in Profile JSON (ADR-010); none of
  those want quoting rules.
- **It is ASCII-only.** Ids are compared byte-wise in the registry
  (`k == id` on `std::string`), so a Unicode id would make
  "same id" depend on the host's encoding assumptions. The grammar
  removes that class of question entirely.

This grammar was chosen against the ids actually in the tree. The list
below is the **authoritative** one — it is what the in-tree factories
return from `id()`, which is what the registry is keyed by (not the
`EngineConfig` default). Every one satisfies the grammar.

| Id | Seam | Returns it via |
|---|---|---|
| `"opus"` | codec, audio-decoder | `NIMRTC_PLUGIN_ID("opus")` |
| `"webrtc_apm"` | audio-3A (the default) | `NIMRTC_PLUGIN_ID("webrtc_apm")` |
| `"webrtc"` | audio-3A (Null stub), RTP, SDP | `NIMRTC_PLUGIN_ID("webrtc")` |
| `"adaptive"` | jitter buffer | `NIMRTC_PLUGIN_ID("adaptive")` |
| `"nimrtc_audio_rx"` | audio receiver | `NIMRTC_PLUGIN_ID("nimrtc_audio_rx")` |
| `"nimrtc_xr"` | XR report consumer | `"nimrtc_xr"` |
| `"nimrtc_net_feedback"` | net-feedback collector | (module default) |
| `"wolfssl"` | DTLS | `NIMRTC_PLUGIN_ID(kBackendId)` |
| `"gmssl"` | DTLS (optional backend) | `NIMRTC_PLUGIN_ID(kBackendId)` |
| `"libsrtp"` | SRTP backend | `"libsrtp"` |
| `"web_transport"` | DataChannel | `NIMRTC_PLUGIN_ID(kBackendId)` |
| `"msquic"` | WebTransport session | `NIMRTC_PLUGIN_ID(kBackendId)` |
| `"h264"`, `"h265"`, `"av1"` | video codec | `NIMRTC_PLUGIN_ID("h264")` etc. |
| `"memory"` | video source | `"memory"` |
| `"headless"`, `"pinned"` | video sink | `"headless"`, `"pinned"` |
| `"reference"` | video sender / receiver | `"reference"` |
| `"ice"` | ICE transport | `"ice"` |
| `"stub"` | SCTP socket | `"stub"` |
| `"arq"` | raw-UDP datagram | `"arq"` |
| `"synthetic"` | video capture | `"synthetic"` |
| `"strict_priority"` | scheduler | (module default) |
| `"aimd"` | BWE | (module default) |
| `"webrtc-classic"`, `"raw-udp-arq"`, `"default"` | transport stack | `NIMRTC_PLUGIN_ID(kBackendId)` |

The grammar was not chosen to fit these — it was **checked** against
them, and every one already conforms. That is the point of choosing a
grammar rather than a convention: a convention is satisfied by
accident, a grammar is satisfied or it is not.

One observation worth recording: the ids split cleanly into the three
Decision 2 categories without any ambiguity. `"webrtc"` appears in
three *different* seams (audio-3A, RTP, SDP) — which is exactly why
the grammar is per-seam and the id is qualified by family in every
diagnostic. The registry slots are per-family, so the same string in
two families is not a collision; it is two independent plugins that
happen to share a name.

### Decision 2 — In-tree ids are namespaced by *provenance*, not by family

An id that names an in-tree implementation is prefixed `nimrtc_`:

- `nimrtc_audio_rx`, `nimrtc_net_feedback`, `nimrtc_xr` — the
  in-tree modules whose names would otherwise collide with a
  conceptual role.

An id that names a **third-party or third-party-shaped** implementation
carries the vendor's own name and carries no `nimrtc_` prefix:

- `"wolfssl"`, `"gmssl"` — vendor crypto libraries.
- `"libsrtp"` — a vendored library (the name is the library's, not
  NimRTC's).
- `"msquic"` — a vendor QUIC stack.

An id that names a **conceptual role rather than an implementation** is
bare and unprefixed:

- `"opus"` — a codec name, which is a protocol-level name (RFC 7587 /
  IANA), not an implementation name.
- `"strict_priority"`, `"aimd"`, `"adaptive"` — algorithms.
- `"memory"`, `"headless"`, `"pinned"`, `"reference"` — roles.
- `"web_transport"`, `"stub"`, `"arq"`, `"ice"` — roles.
- `"webrtc"` — the WebRTC *specification's* name for a behaviour
  ("the WebRTC jitter buffer", "the WebRTC audio processing module"),
  used by the audio-3A stub, the RTP layer and the SDP layer.

The prefix is therefore a **claim about ownership**, and it is the
thing that lets a reader of a config file tell "this is the thing we
ship" from "this is something a customer plugged in". A third-party
plugin **must not** use the `nimrtc_` prefix: it would be claiming a
name that belongs to the project.

### Decision 3 — Third-party ids carry their own namespace

A third-party plugin id must be qualified by the plugin's own
namespace, so that two vendors shipping an "opus" codec cannot collide
in the same registry slot:

```
<namespace>_<implementation>
```

for example `acme_h264`, `contoso_3a`, `vendorx_capture`. The namespace
segment is the vendor's name; the implementation segment follows
Decision 1's grammar.

This is the mechanism that lets the registry remain a single flat
per-family map without needing a nested namespace type. It also makes
collisions *visible*: two plugins claiming `acme_h264` are visibly
fighting over the same name, rather than silently shadowing each
other across families.

### Decision 4 — The registry rejects malformed ids at registration time

`TypedRegistry::register_one` currently accepts anything, including
the empty string. This ADR requires that registration validate the id
against the Decision 1 grammar and **fail loudly** on a violation.

The registration happens at **static-init time**, which rules out
throwing (a throw from a static initialiser terminates the process
before `main`, with a message that names no plugin). The validation
therefore reports through the engine's diagnostic path, and the
contract for the host is:

- An id that violates the grammar **must not** be registered.
- The registration attempt **must** produce a diagnostic that names
  the offending id and the family.
- The rest of the registry is unaffected — one bad plugin does not
  poison the other families.

At registration time there is no engine instance to report through, so
the diagnostic goes to the core logger. The existing
`TypedRegistry::register_one` overwrite-on-duplicate behaviour is
unchanged by this ADR: a duplicate id is still a late-binding
overwrite, not an error. (Last registration wins. That is deliberate —
it is what lets a host install an override for a built-in — but it
means a duplicate is *not* caught by validation, and the diagnostic
path for it is the overwrite itself.)

### Decision 5 — The `NIMRTC_REGISTER_*` macros accept one spelling only

The two-flavour macro inconsistency described in the Context section is
closed. **All** `NIMRTC_REGISTER_<FAMILY>` macros take the id as a
**bare identifier token** and stringify it themselves:

```cpp
NIMRTC_REGISTER_DTLS_SESSION(mybackend, &g_factory);        // -> "mybackend"
NIMRTC_REGISTER_AUDIO_DECODER(opus, &g_opus_decoder_factory); // -> "opus"
```

Passing a quoted literal is a **compile error**, enforced by a
`static_assert` in the macro body that rejects a token whose spelling
begins with `"`. This matters because the legacy macros already
stringify, and a user who writes `NIMRTC_REGISTER_DTLS_SESSION("x", &f)`
today gets a *registered but unmatchable* id `"\"x\""` — a silent
failure that looks like a working registration.

The audio-seam macros change from literal-as-is to token-and-stringify.
This is a source-level change for any out-of-tree caller that passed a
literal, and is therefore a **breaking change within v1.2.0** for
third-party audio-seam plugins only; the in-tree callers are updated in
the same change.

**The one exception: hyphenated ids.** Decision 1 permits the hyphen,
and the `#id` stringification form cannot produce one — `#my-id` does
not lex as a single token. A plugin whose id contains a hyphen must
therefore **not** go through a registration macro at all; it calls the
registry directly:

```cpp
// The direct form: the id is passed through as-is.
core::PluginRegistry::instance().register_transport_stack("webrtc-classic", &g_factory);
```

This is exactly what the in-tree transport module does today — see
`src/modules/transport/src/transport_plugin.cpp`, whose registrars call
`register_transport_stack(std::string_view{f->id()}, f)` with the
hyphenated ids `"webrtc-classic"` and `"raw-udp-arq"`. No in-tree code
uses `NIMRTC_REGISTER_TRANSPORT_STACK` at all.

So the resolution of the original two-flavour inconsistency is not
"one spelling wins" but **"one spelling per capability"**: the
tokenising form is the default and the only form that can produce a
duplicate-token check, and a hyphenated id — which the tokenising form
cannot express — is registered through the registry directly, which is
the same path the in-tree default registrars use.

What the two-flavour inconsistency did **not** have was a *rule*; this
ADR supplies the rule (token by default, direct registry call where
the token form cannot express the id) and, critically, the
compile-time `static_assert` that makes the quoted-literal mistake in a
tokenising macro an **error** instead of a silent unmatchable
registration.

The macros remain the *preferred* registration path for third parties,
because they are the only path that gets the duplicate-token check.
Direct calls to `core::PluginRegistry::register_<family>("<id>", &f)`
remain supported — they are how the in-tree default registrars work
— but a third-party plugin that calls the registry directly takes
responsibility for the grammar itself.

### Decision 6 — The compatibility promise for an id

An id, once published, is a **stable public string** for the lifetime
of the major version. Specifically:

- **Adding** a new id is additive and non-breaking.
- **Renaming** an id is a breaking change to the public config
  surface. It requires a deprecation cycle: the old id and the new id
  are both registered for at least one minor version, and the old id
  is logged at `Warn` when it is selected.
- **Removing** an id is a breaking change and requires the same
  deprecation cycle, plus a minor-version bump.
- The **grammar** (Decision 1) and the **namespace rules**
  (Decisions 2–3) are part of the contract. An id that violates them
  is not a published id, and may be rejected without a deprecation
  cycle.

This restates the plugin author guide's §8 promise in a form that can
be tested: the deprecation cycle is observable as "both ids resolve to
the same factory".

### Decision 7 — The contract is enforced by a test, not by prose

The grammar, the rejection behaviour, and the macro spelling are
verified by `tests/test_plugin_id_contract.cpp` (added by this
change), which asserts:

1. Every id registered by the in-tree default registrars satisfies the
   Decision 1 grammar. This is the regression test that keeps the
   in-tree ids honest as modules are added.
2. A malformed id (empty, leading digit, uppercase, embedded space,
   non-ASCII) is rejected by the registry. The hyphen is **not** in
   this list — Decision 1 permits it, and the shipped
   `"webrtc-classic"` / `"raw-udp-arq"` ids must keep registering.
3. A rejected id does not remove or shadow a previously-registered id
   in the same slot.
4. The registration macros reject a quoted-literal id at compile time
   (a `static_assert` — the test asserts the *macro* rejects it, and
   the compile-time rejection is itself verified by the fact that the
   test file compiles).

### Decision 8 — The in-tree default registrars use the same spelling rule

`src/core/src/pal_default_registrars.cpp` does not use the
`NIMRTC_REGISTER_*` macros — it calls
`core::PluginRegistry::register_<family>("<id>", &factory)` directly,
so in-tree ids and third-party ids travel through two different code
paths.

This ADR requires that the **in-tree default registrars adopt the
same grammar** as third-party plugins, and that they be covered by the
same test (Decision 7, item 1). They do not need to adopt the same
*mechanism*: calling `register_<family>("opus", &f)` directly is
fine, and is the reason the in-tree ids do not go through a
stringifying macro. What matters is that both paths land in the same
validated registry, so a malformed in-tree id is caught by the same
check as a malformed third-party one.

The macros remain the **preferred** path for third-party plugins,
because they are the only path that gets the duplicate-token check
(Decision 5). A third-party plugin that calls the registry directly
takes responsibility for the grammar itself.

### Decision 9 — `NIMRTC_PLUGIN_ID` becomes the compile-time grammar gate

The codebase already has a compile-time id wrapper:
`nimrtc/core/plugin_id.hpp` provides `NIMRTC_PLUGIN_ID(x)`, which
wraps an id literal in a `PluginIdTag<__LINE__>`-typed value. Every
in-tree factory's `id()` returns through it:

```cpp
std::string_view OpusPluginFactory::id() const noexcept {
    return NIMRTC_PLUGIN_ID("opus");
}
```

It exists for a *different* reason — making each callsite greppable
and its type unique — but it is the natural home for this ADR's
grammar check, and reusing it avoids a second id-validation mechanism.

Two gaps are closed:

1. **The non-empty check does not exist.** `plugin_id.hpp` documents
   that "a `static_assert` at the callsite validates that the literal
   is non-empty", and three other headers repeat the claim
   (`raw_udp_factory_iface.hpp`, `dtls_session_factory.hpp`,
   `dtls_wolfssl_factory.cpp`). The `PluginIdTag` constructor body is
   **empty** — the check is documented but was never written. This
   ADR requires it to be written, and to be extended from
   "non-empty" to "matches the Decision 1 grammar", which is a
   strict superset.
2. **The grammar check belongs on the wrapper, not on the
   registry.** A `constexpr` validator on the literal can check the
   *spelling* at compile time, with no diagnostic channel to worry
   about. The registry-side check (Decision 4) then only has to
   handle ids that arrived by some other route — a direct
   `register_<family>("...", &f)` call, or a `std::string_view`
   computed at runtime.

The result is two layers, each catching what the other cannot:

| Layer | Catches | Diagnostic |
|---|---|---|
| `NIMRTC_PLUGIN_ID` — compile-time | spelling: empty, uppercase, leading digit, non-ASCII, or any character outside the grammar | compile error, at the callsite |
| `PluginRegistry::register_one` — runtime | ids that bypass the macro (direct registry calls, runtime-computed ids) | log, at registration |

A plugin author who uses `NIMRTC_PLUGIN_ID` (or the registration
macros, which will use it) gets a **compile-time** error for a
malformed id. An author who calls the registry directly gets a
**runtime** diagnostic. Neither gets silence.

**Note on `PluginIdTag<__LINE__>` uniqueness.** The wrapper's
type-uniqueness is a *callsite* property, not a *value* property —
two modules that both write `NIMRTC_PLUGIN_ID("opus")` produce two
distinct types with the same string value, and the registry still
sees two registrations of the same id ("last writer wins"). The
wrapper makes the collision **greppable**; it does not prevent it.
The value-level invariant remains the runtime one described in
Decision 4.

## Consequences

### Positive

- **A typo is caught at the point of the typo.** Today, `cfg.dtls_name = "wolfsl"`
  produces a `kEngineInvalidParam` at `open()` with a message that does
  not distinguish "typo" from "plugin not linked into this binary".
  Under Decision 4 the id is validated where it is written, and the
  registry additionally refuses to hold an id it could never match.
- **Config files become greppable.** With Decision 2's prefix rule, a
  reader of a config file can see at a glance which entries are
  in-tree defaults and which are site-specific, and `grep '^nimrtc_'`
  finds exactly the NimRTC-provided components.
- **The two macro flavours no longer silently disagree.** The quoted
  -literal-in-a-legacy-macro failure mode — a registration that
  succeeds and never resolves — is eliminated by Decision 5.
- **The deprecation cycle becomes testable.** "Both ids resolve" is an
  assertion, not a convention.

### Negative / costs

- **The audio-seam macros are source-breaking** for out-of-tree callers
  that passed a string literal (Decision 5). This is a deliberate
  v1.2.0 breaking change, consistent with the ADR-015 policy, and it
  is the only breaking change this ADR introduces.
- **Validation costs a little at static-init.** The grammar check is a
  short scan of a short string, once per plugin, before `main`. This is
  not measurable against the cost of the static initialisation itself.
- **Two new sources of truth.** The grammar lives in the registry
  (Decision 4) and in the test (Decision 7), and the prose in the
  plugin author guide must match both. This is the usual cost of a
  contract that is meant to be enforced rather than merely described.

### Neutral

- No engine behaviour changes. The resolution semantics of §4.1 of the
  plugin author guide — hard failure / reported / logged / disabled —
  are unaffected. This ADR governs *how an id is spelled and validated*,
  not *what happens when a family cannot be resolved*.

## Alternatives considered

### A. Validate ids only in `EngineConfig` / at engine `open()`

Rejected. It catches the typo but not the malformed registration, and it
puts the diagnostic in the one place where the host is already being
told something else went wrong. The registration site is the earlier
and better place: it is where the mistake was made.

### B. Use a typed ID (`struct PluginId`) instead of a raw string

Rejected for v1.x. A typed ID would let the compiler check spelling, but
it would also change the public shape of `EngineConfig` — every
`*_name` field becomes `PluginId` — which is a much larger ABI break
than this ADR's scope, and it would force every host to construct IDs
through a builder API. The grammar check achieves most of the benefit
at a fraction of the cost. This remains the natural direction for a
future v2 if the string surface is ever reworked.

### C. Use the full `namespace::implementation` form

Rejected. `::` is not valid in the `#id` stringification form, so the
legacy macros could not express it, and it would break the "a valid
C++ identifier" property that makes the macro contract simple.
Underscore-separated is sufficient, and the `nimrtc_` prefix already
reserves the in-tree namespace.

### D. Leave the grammar to each family

Rejected. A per-family rule set means the host must learn N different
rules, and the "is this a valid id" question has no single answer for
the registry to check.

## References

- `docs/adr/ADR-001-plugin-system.md` — the string-id seam decision.
- `docs/adr/ADR-010-profile-json-format.md` — JSON is a first-class
  config format; ids appear in it.
- `docs/adr/ADR-015-api-stability-policy.md` — the stability promise
  this ADR's Decision 6 specialises for plugin ids.
- `docs/plugin_author_guide.md` §4 (catalogue), §8 (versioning), §9
  (pitfalls).
- `docs/plan/architecture-remediation-plan.md` §7.1, §7.2 — the
  remediation plan's Phase 5 scope.
