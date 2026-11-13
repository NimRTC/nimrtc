# ADR-026: The scheduler's C ABI — a real `extern "C"` surface

| | |
|---|---|
| Number | 026 |
| Status | **Accepted** |
| Date | 2026-11-13 |
| Phase | Architecture remediation, Phase 4 §4.4 (item D5) |
| Bound to | v1.2.0 |
| Supersedes | — |
| Related | `docs/plan/architecture-remediation-plan.md` §4.4 (D5), [ADR-015](ADR-015-api-stability-policy.md) (the stability / ABI policy), [ADR-025](ADR-025-sdp-rtp-not-pluggable.md), `src/modules/sched/include/nimrtc/sched/sched_c.h`, `src/modules/sched/include/nimrtc/sched/sched.hpp` |

## Context

The architecture remediation plan's **D5** item asked for the scheduler's
"C ABI" to be documented as the embedded-host entry points, and for each
entry point's users to be verified.

The verification turned up a problem: **the "C ABI" was not a C ABI.** The
section banner in `src/modules/sched/include/nimrtc/sched/sched.hpp` read:

> ```
> // C ABI wrapper — opaque handle + C linkage for FFI consumers
> //
> // These five entry points are the **S-family (per-stream) interface for C
> // hosts** — the embedded / FFI consumers who cannot use the C++ API.
> ```

Neither claim was true.

### The evidence

**1 — A C compiler cannot consume the header.** The header includes
`<cstdint>`, `<functional>`, `<memory>`, `<optional>`, and `<vector>`, and
`nimrtc/core/api_version.hpp` declares things inside `namespace nimrtc`.
A `/TC` compile of a five-line C translation unit that includes the header
and calls `sched_create()` fails immediately:

```
api_version.hpp(208): error C2061: syntax error: 'nimrtc'
sched.hpp(114): fatal error C1083: cannot open include file: 'cstdint'
```

**2 — The symbols are C++-mangled.** `dumpbin /symbols` on the built
archive reports:

```
?sched_create@sched@nimrtc@@YAPEAUSchedulerHandle@12@PEBUSchedulerConfig@plugins@2@@Z
?sched_destroy@sched@nimrtc@@YAXPEAUSchedulerHandle@12@@Z
?sched_select_next_packet@sched@nimrtc@@YAHPEAUSchedulerHandle@12@HP6AXIW4StreamPriority@plugins@2@PEBE_KPEAX@Z4@Z
```

A C host cannot link these by name.

**3 — The types are C++ types.** `plugins::SchedulerConfig` is a C++
aggregate; `plugins::StreamPriority` is a scoped `enum class : uint8_t`
with no C spelling; `plugins::Addr` is a C++ struct with a defaulted
member initializer.

**4 — There was no C API anywhere in the tree.** The only `.h` file in the
whole non-vendored tree was `src/core/include/nimrtc/core/nimrtc_export.h`
— a DLL-export macro header, not an API header. `extern "C"` appeared
only in three *private* `web_transport` implementation headers.

### The zero-call audit

Every one of the six entry points was referenced by exactly **one** file:
`src/modules/sched/tests/test_priority.cpp`. No production path, no
example, no other module. They were reachable only from a downstream
consumer linking `nimrtc::sched` directly.

The plan's D5 anticipated that some entry points might be unused and
deletable. They were **not** deleted — but for a reason stronger than the
plan's own: ADR-015 marks them `NIMRTC_API_STABLE`, so removing them
requires a MAJOR bump plus a deprecation cycle. They are a
**commitment**, not an accident.

### Why that makes the false label worse, not better

A `NIMRTC_API_STABLE` symbol that a downstream integrator is told is "the
C ABI" is a promise about a capability. The header **ships** in
`<prefix>/include/`. An integrator who reads it and wires an FFI layer
hits three separate compile/link failures and no explanation — the
documentation promised something the library did not deliver.

That is precisely the "功能已具备的错觉" (the illusion of a capability that
is not there) that the remediation plan's D-class items exist to
eliminate. It is the same defect as a dead module with a full
implementation and a passing test suite.

## Decision

**NimRTC gains a real C ABI for the scheduler's S-family, and the C++
entry points become forwarders to it.**

### Decision 1 — A new, genuine `extern "C"` header

`src/modules/sched/include/nimrtc/sched/sched_c.h` is a pure C11 header:

- `extern "C"` linkage (guarded by `#ifdef __cplusplus`);
- plain C types only — `uint32_t`, `int32_t`, `size_t`, `uint8_t`;
- no dependency beyond `<stdint.h>` and `<stddef.h>`;
- an opaque `NimrtcSchedHandle` (the layout stays hidden);
- flat C mirrors of the two structs — `NimrtcSchedConfig` and
  `NimrtcSchedAddr`;
- an explicit visibility attribute (`NIMRTC_SCHED_C_API`) so the symbols
  survive the project's `-fvisibility=hidden` on ELF platforms;
- a version marker (`NIMRTC_SCHED_C_ABI_VERSION_MAJOR` / `_MINOR`) so a
  C host can assert the library is new enough to carry the ABI at all.

### Decision 2 — One implementation, two spellings

The six C++ functions in `sched.hpp` are retained **unchanged in
signature and semantics** (they are `NIMRTC_API_STABLE`), and are now
implemented as **thin forwarders** to the C ABI functions.

The C ABI is the implementation; the C++ spelling is the compatibility
layer. There is exactly one copy of the logic, so the two spellings
cannot drift — the same discipline the plan's D2 item demands for the SDP
format parameters.

`nimrtc::sched::SchedulerHandle` becomes an alias of the C ABI's
`NimrtcSchedHandle`, so the C++ type identity is preserved: a handle
obtained through either spelling is the same type and the same object.

### Decision 3 — The C ABI is the safer of the two, and it says so

The C surface validates rather than trusts:

- an out-of-range priority level is **rejected**, not clamped — the
  scheduler's bucket count is the authority, and a level outside it would
  index past the bucket array;
- a NULL handle is a no-op on every entry point;
- a negative `max_packets` drains nothing;
- `nimrtc_sched_create(NULL)` means "the library defaults", which is
  explicitly **not** the same as a zeroed `NimrtcSchedConfig` (the C++
  defaults are `protect_keyframes = true`, `enabled = false`). The C
  header states this, because a C host's instinct is to zero the struct.

### Decision 4 — The veto difference is documented, not hidden

`plugins::StreamDrainCallback` returns `bool` (false = stop draining and
keep the packet queued). The drain callback in **both** spellings returns
`void`, so **the function-pointer shape cannot veto** — it always drains
the full batch. The C header documents this on the entry point.

The stream identity and the bucket priority **are** preserved across the
boundary: the callback receives the real stream id and the real bucket
level, not placeholders. The C++ forwarder adapts the scoped enum to the
C `int32_t` through a **lambda**, not a `reinterpret_cast` between two
different function-pointer types — which would be undefined behaviour.

### Decision 5 — A C-language test is the actual assertion

`src/modules/sched/tests/test_sched_c_abi.c` is compiled as **C11**, not
C++. This is the load-bearing part of the decision: a C++ test cannot
prove the claim, because in a C++ translation unit `extern "C"` is
redundant and every C++ type is available — it would happily compile
against an ABI no C compiler could use.

Compiling that file as C **is** the assertion. If the C header ever
grows a C++-only dependency (a `namespace`, a reference type, a default
argument), the file stops compiling — which is exactly the regression the
test exists to catch.

The behavioural assertions are deliberately thin; the C++ test
(`test_priority.cpp`) already covers the S-family semantics. What the C
test checks is the **boundary**: that the symbols link, that the opaque
handle survives a round trip, that the priority levels cross with their
documented numeric values (High = 0, Normal = 1, Low = 2), that an
out-of-range level is rejected, that the callback receives the real stream
id and priority, and that a NULL handle is tolerated everywhere.

## Consequences

### What gets better

- **The capability is real.** A C or Rust host can now include
  `<nimrtc/sched/sched_c.h>`, call `nimrtc_sched_create()`, and link.
  The C test proves it on every CI leg.
- **The shipped header stops lying.** The false "C linkage" / "for C
  hosts" claim is replaced by a header that describes the actual
  situation — and by a second header that delivers the promise.
- **No drift.** The two spellings share one implementation by
  construction, not by discipline.
- **The zero-call audit is resolved.** The six C++ entry points now have
  a *reason* to exist (they are the compatibility spelling) and the C ABI
  has a *purpose*. Neither is a set of symbols nobody can call.
- **The embedded-host scenario is served properly.** An embedded host can
  now use the S-family without pulling the `plugins::IScheduler` vtable
  into its translation unit — which is what the opaque handle was always
  for.

### What gets worse, honestly

- **The public surface grew by one header.** `sched_c.h` is a new
  installed public header, so the API-classification baseline moves (the
  count test requires the baseline bump in the same change). The C ABI is
  also a new stability commitment: the struct layouts and the numeric
  constants are now ABI.
- **Two names for the same thing.** `sched_create` and
  `nimrtc_sched_create` exist side by side. That is the cost of keeping a
  `NIMRTC_API_STABLE` C++ surface while adding a real C one, and it is
  why the C++ side is explicitly labelled the *compatibility* spelling.
- **The C ABI duplicates the type definitions.** `NimrtcSchedConfig` and
  `NimrtcSchedAddr` are C mirrors of C++ structs, and they must be kept in
  sync. The conversion helpers live in one place
  (`scheduler_c_impl.hpp`) and are exercised by the C test.

### Follow-up work

| Item | Where | Status |
|---|---|---|
| Extend the C ABI to the other seams (transport, DTLS, SRTP) | — | Not started — one seam first, to learn the shape |
| A CI guard that a `.h` under `src/**/include/` is C-clean | Phase 5 | Pending |
| ADR-023 / ADR-024 (plugin ID naming, `EngineConfig` string ownership) | §7.2 | **Done** (ADR-023 Decisions 4/5/7 landed in the Phase 4 branch; ADR-024's `std::string_view` → `std::string` migration is complete — `EngineConfig` has zero `string_view` members) |
| The 8 documents in §7.1 | §7.1 | Pending |

## Alternatives considered

### A — Delete the six functions as dead code

Rejected. They are `NIMRTC_API_STABLE`; ADR-015 forbids removal without a
MAJOR bump and a deprecation cycle. The zero-call audit says "no internal
caller", not "no caller" — and the surface was *promised* as the C entry
point, which is a reason to make the promise true rather than to break
it.

### B — Relabel the header and stop calling it a C ABI

Rejected. This was the first instinct and it is a real option: a
comment-only fix that says "these are C++ opaque-handle wrappers, not a C
ABI". It removes the false promise, and it is strictly less work.

Rejected because it leaves the *capability* missing. The plan's premise
for retaining these entry points was that the C ABI is "the stable way for
an embedded host to reach the per-stream priority family". Retaining
entry points is only worthwhile if they are callable from C — otherwise
the project keeps an embedded-host surface that no embedded host can use,
which is the same illusion, one layer down.

### C — Rename the C++ functions to signal the change

Rejected. Renaming a STABLE symbol is itself an ABI break, and the names
are not the problem — the *implementation* was.

### D — Make the existing C++ functions `extern "C"` in place

Rejected. It would change the mangled names of six STABLE symbols — an
ABI break — and it would still not work: the parameter types
(`plugins::SchedulerConfig*`, the scoped `enum class`) cannot be named by
a C translation unit. Linkage alone does not make a C ABI.

## Verification

| Check | Where | Required |
|---|---|---|
| The C header compiles as C11 and the C test passes | `modules_sched_test_sched_c_abi` (C-language source) | green on Windows / Linux / macOS |
| The C++ entry points still work unchanged | `modules_sched_tests` (`test_priority.cpp`) | green |
| The symbols are unmangled and exported | `dumpbin /symbols` / `nm -D` on the archive | `nimrtc_sched_create` present, no `?` prefix |
| The whole suite is green | `ctest` | green |
| The public-header count moved by exactly one | `tests/test_api_classification.cpp::total_header_count_matches_baseline` | baseline bumped in the same change |
| Every public header that uses a stability marker includes `api_version.hpp` | `scripts/check_api_marker_includes.py` | green |
| No public symbol lost | `scripts/api_freeze_scan.js` | `Missing: 0` |

## Out of scope (this ADR)

- **A C ABI for the other seams** (transport, DTLS, SRTP, the engine
  itself). This ADR establishes the *shape*; extending it is separate
  work, and doing it for every seam at once would be a large, untested
  surface change.
- **The D1 engine split** (remediation plan §4.3) — the engine's `Impl`
  is 1791 lines against a <400-line target.
- **The L4 exception for RTP / SDP** — that is
  [ADR-025](ADR-025-sdp-rtp-not-pluggable.md)'s subject.

## References

- [`docs/plan/architecture-remediation-plan.md`](../plan/architecture-remediation-plan.md) —
  the parent plan; §4.4 (D5) is the item this ADR closes.
- [`docs/adr/ADR-015-api-stability-policy.md`](ADR-015-api-stability-policy.md) —
  the stability classification and ABI-commitment policy that makes the
  six `sched_*` symbols un-removable without a MAJOR bump.
- [`docs/adr/ADR-025-sdp-rtp-not-pluggable.md`](ADR-025-sdp-rtp-not-pluggable.md) —
  the sibling Phase 4 ADR: how a *not*-pluggable module is handled
  honestly.
- [`src/modules/sched/include/nimrtc/sched/sched_c.h`](../../src/modules/sched/include/nimrtc/sched/sched_c.h)
  — the real C ABI.
- [`src/modules/sched/tests/test_sched_c_abi.c`](../../src/modules/sched/tests/test_sched_c_abi.c)
  — the C-language test that is the load-bearing assertion.
