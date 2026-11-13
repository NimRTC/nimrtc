# ADR-025: SDP and RTP are not pluggable — and the engine's concrete includes are enumerated, not hidden

| | |
|---|---|
| Number | 025 |
| Status | **Accepted** |
| Date | 2026-11-13 |
| Phase | Phase 4 (v1.2.0) — structure remediation |
| Bound to | v1.2.0 |
| Supersedes | — |
| Related | `docs/plan/architecture-remediation-plan.md` §2.1 (question Q2), §4.5 (item D6); `AGENTS.md` L4; `ARCHITECTURE.md` Layout Invariant 4; `src/engine/src/engine_impl.hpp`; `src/modules/sdp/`; `src/modules/rtp/` |

## Context

Layout Invariant **L4** in `AGENTS.md` says:

> `src/engine/` only orchestrates plugins; it does not directly include any
> concrete module's header.

`ARCHITECTURE.md` (Layout Invariant 4) states the same thing in slightly
different words. The intent is sound: the engine is the *composition*
layer, and if it reaches into a concrete implementation, the seam stops
being a seam and the module can no longer be swapped.

But the invariant is **not what the code does**, and it has not been
true for a long time. `src/engine/src/engine_impl.hpp` includes:

```cpp
#include <nimrtc/sdp/session_description.hpp>   // sdp::Parser, sdp::Munger
#include <nimrtc/rtp/packet.hpp>                 // rtp::PacketBuilder
#include <nimrtc/rtp/rtcp.hpp>                   // rtp:: RTCP demux
#include <nimrtc/rtp/timeline.hpp>               // ADR-017 timeline seam
#include <nimrtc/audio3a/audio3a.hpp>
#include <nimrtc/h264/codec_plugin.hpp>
#include <nimrtc/dtls/dtls.hpp>
#include <nimrtc/srtp/srtp.hpp>
```

The counts across the engine's translation units are substantial: `sdp::`
appears 120 times (`engine_signaling.cpp` alone accounts for 76), `rtp::`
appears 72 times spread over 10 files.

The architecture remediation plan (§2.1, question Q2) raised the question
of whether SDP and RTP should become pluggable like the other seams, and
the decision was closed as **Q2: do not replace them for now** — delete the
fake configuration options and mark the interfaces deprecated.

That left the invariant in an awkward state. There were three ways to
resolve it, and this ADR picks the one that does not require pretending:

1. **Force the abstraction.** Write an `ISdpBackend` / `IRtpBackend` seam
   and route every engine call through it.
2. **Violate the invariant silently.** Leave the code as it is and let the
   documentation drift.
3. **Change the invariant to match reality, explicitly and narrowly.**

## Decision

**SDP and RTP are *not* pluggable, and the engine is allowed — and
expected — to use their concrete implementations directly. L4 is rewritten
to say so, with the exception scoped as narrowly as the reality allows.**

The same rewriting also brings the engine's *other* concrete includes
(`ice::`, `dtls::`, `srtp::`, `audio3a::`, `transport::`,
`web_transport::`) under the explicit, enumerated form of the rule. They
were already there; what changes is that the rule now **names** them
instead of being silently violated.

### Decision 1 — The exemption is scoped, and it names the modules it covers

The rewritten L4 exempts a **named, enumerated set** of modules, not "the
engine" and not "SDP and RTP". The set is exactly the concrete module
headers the engine's *internal* implementation files include today:

| Module | Why it is included | What the exemption buys |
|---|---|---|
| `sdp::` | The session description is the contract every other component is written against | The negotiation model cannot be replaced without replacing the negotiation |
| `rtp::` | The packet builder is on the zero-allocation hot path | A virtual call and an interface-hidden scratch buffer per frame |
| `ice::` | The ICE transport's concrete state and role enums | — |
| `dtls::` | The DTLS session's address / role / state types | — |
| `srtp::` | The SRTP context's crypto-suite enum | — |
| `audio3a::` | The 3A pipeline's config and frame types | — |
| `transport::` | The transport-stack / selector / strategy types | — |
| `web_transport::` | The WebTransport session's config and factory types | — |

Every *other* concrete module — the codec modules (`h264` / `h265` / `av1`),
the video pipeline, the jitter buffers, the BWE implementations, the pacing
module, the scheduler — is reached through its `plugins::I*` seam, and the
engine does **not** include its header.

This is the narrowest exemption that is actually true of the code. A
blanket exemption would remove the constraint entirely and give back the
problem the invariant exists to prevent.

> **Note on `h264::`** — `engine_impl.hpp` includes `<nimrtc/h264/codec_plugin.hpp>`
> behind `#ifdef NIMRTC_HAS_H264`, but it uses no symbol from it: the
> include exists so the H.264 plugin's registration is visible to the
> linker. That include is a **link-time registration convenience**, not a
> type dependency, and it is scheduled for removal when the engine's
> default-registrar table (D1) is split out of `engine_impl.hpp`.

### Decision 1a — The seam-only modules remain the rule

The modules listed in the table above are the *only* concrete module
headers the engine may include. The rule for everything else is unchanged:
reach it through `plugins::I*`, never include its header.

This is what makes L4 enforceable: the question "does `src/engine/`
include anything outside the table and the `plugins::` seams?" has a
finite answer, and a reviewer can check it mechanically. Before this ADR,
L4 was a rule the code violated on every screen, so it had no reviewing
power at all.

### Decision 2 — The rationale is performance, and it is a real one

The engine's send path is on the hot path for every frame and every audio
packet. Phase 3 (§3.5) established a hard gate: **zero heap allocations
per frame** in `send_video()` steady state at 1080p30, and the same for
the audio path.

An abstract `IRtpBackend` with a virtual `build_packet()` would put a
virtual call on that path, and — more importantly — would force the
packet-builder's internal scratch state behind an interface boundary that
a caller cannot see. The current `rtp::PacketBuilder` lets the engine
pre-size its scratch buffers once and reuse them across frames, which is
exactly what makes the zero-allocation gate pass.

The same applies to SDP: the engine's offer/answer path builds and re-parses
the same session description several times, and `sdp::Munger`'s attribute
model is a value type that the engine holds directly.

### Decision 3 — The fake replaceability is removed, not papered over

Before this decision, `EngineConfig` carried configuration options that
implied SDP and RTP were replaceable — `sdp_name`, `rtp_name`. Those
options were removed in Phase 1 as part of the ABI-breaking change
recorded in the v1.2.0 migration notes, and `src/plugins/include/nimrtc/plugins/sdp.hpp`
is now marked `@deprecated — Not pluggable as of v1.2.0`, with the
reasoning spelled out in the header.

This ADR completes that removal at the *documentation* level: the
remaining seam interfaces are documented as not being a plug point, so a
consumer does not write a plugin against them.

### Decision 4 — The `rtp::` usage is concentrated, and that is a tracked goal

The plan (§4.5) requires the `rtp::` usage to be concentrated into a single
engine file — `src/engine/src/rtp_packetizer.cpp` — so that the exception
has a visible boundary.

This is **not yet done**: the `rtp::` usage currently spans 10 engine
files. It is a prerequisite for the D1 engine split (§4.3), which is the
work that extracts the packetizer into a collaborator class. This ADR
records the requirement; the concentration lands with D1.

## Consequences

### What gets better

- **The invariant becomes enforceable again.** A reviewer can now check
  L4 mechanically: "does `src/engine/` include anything outside
  `sdp::` / `rtp::` and the `plugins::` seams?" — a question with a
  finite answer. Before this ADR, L4 was a rule the code violated on every
  screen, so it had no reviewing power at all.
- **The documentation stops lying.** `ARCHITECTURE.md` Layout Invariant 4
  described an engine that does not include concrete module headers. That
  was never true.
- **No performance regression.** The alternative (a real seam) would have
  put virtual calls and interface-hidden state on the hot path, against
  the zero-allocation gate that Phase 3 just established.

### What gets worse, honestly

- **SDP and RTP are now genuinely non-replaceable.** A consumer who wants a
  different SDP implementation or a different RTP stack cannot have one
  through the engine. That is the price, and it is the price the Q2
  decision already accepted.
- **The exemption is a coupling, and couplings rot.** The mitigation is
  the narrow scope (Decision 1) plus the concentration requirement
  (Decision 4).

### Follow-up work

| Item | Where | Status |
|---|---|---|
| Concentrate the `rtp::` usage into `rtp_packetizer.cpp` | §4.3 / D1 | Pending (blocked on the engine split) |
| Mark the residual SDP seam interface deprecated | §2.1 / Q2 | **Done** (`plugins/sdp.hpp`) |
| Add a CI guard that flags new concrete-module includes in `src/engine/` | Phase 4 | Pending |

## Alternatives considered

### A — Write a real `ISdpBackend` / `IRtpBackend` seam

Rejected. The hot-path argument (Decision 2) is decisive: the zero-
allocation gate is a hard requirement, and the seam would work against it.
A seam that is added and then worked around is worse than no seam: it
creates the "functional capability exists" illusion the remediation plan
calls the worst possible state for a dead module.

### B — Move the SDP/RTP calls out of the engine entirely

Rejected. The engine *is* the component that builds an offer, answers an
answer, and packetises media — moving that logic into a module would
either recreate the engine elsewhere or leave the engine as a shell that
delegates everything, which contradicts its purpose.

### C — Leave L4 alone and accept the drift

Rejected. An invariant that is violated everywhere is worse than no
invariant, because it teaches reviewers to ignore it.
