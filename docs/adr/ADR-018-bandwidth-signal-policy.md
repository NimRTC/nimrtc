# ADR-018: Bandwidth signal policy — transport-cc preferred, loss/RTT fallback

| | |
|---|---|
| Number | 018 |
| Status | **Accepted (landed)** |
| Date | 2026-11-09 |
| Phase | v1.2.0 (M1 — the feedback ground) |
| Bound to | v1.2.0 |
| Supersedes | — |
| Related | `docs/plan/video-pipeline-v2-plan.md` §0 D1, Track F (F1–F8), §B.2 (M1/M2), `docs/adr/ADR-015-api-stability-policy.md`, `docs/adr/ADR-001-plugin-system.md`, `src/plugins/include/nimrtc/plugins/bwe.hpp`, `src/modules/bwe/` |

## Context

Decision **D1** in `docs/plan/video-pipeline-v2-plan.md` §0 settled the question
of *which* bandwidth signal drives the sender:

> **D1 — Bandwidth signal: support both, compatible.** Use `transport-cc`
> when the peer supports it; fall back to loss/RTT (REMB / RR) otherwise.

The decision names the outcome. It does not say **how** the two signals are
selected, **who** selects them, **what** happens when both are present, or
**what** the wire formats actually are. Those four gaps are what this ADR
closes, because the first milestone of v1.2.0 (M1 — the feedback ground)
cannot be built on an unresolved policy: M1 has to decide *which* RTCP packets
the engine parses, and that decision is the policy.

### The state of the code today

The gap is not a missing feature; it is a missing **connection**.

* `plugins::IBwe::on_feedback()` is called **only from unit tests**. Nothing in
  the production path feeds it. (`docs/plan/video-pipeline-v2-plan.md` Track F
  preamble.)
* The engine's `tick()` reads `bwe_->estimate()` and hands it to
  `scheduler_->on_bwe_update()`. The estimate therefore stays pinned at
  `initial_bitrate_bps` (1 Mbps default) forever. The scheduler shapes the
  *send schedule* to that number, but nothing tells the *encoder* to change
  its bitrate — so the video picture does not get worse when the network does,
  it just gets dropped on the floor.
* `rtp::Parser::parse_transport_cc()` exists, is unit-tested, and has **no
  caller outside tests**. The engine's RTCP walk (`handle_rtcp()` in
  `src/engine/src/engine_media.cpp`) recognises only `PT == 200` (SR); every
  other RTCP sub-packet is stepped over by its length field and discarded.
* The engine *does* already receive a receiver report for its own inbound
  audio stream — but only to *send back* one. The inbound RR's loss and RTT
  figures are parsed, used for the local report, and then dropped.

So today there is exactly one place where a network opinion could enter the
system, and it is not wired.

### The two signals are not interchangeable

They measure different things, and this drives every rule below.

| | **transport-cc** (RTPFB FMT=15) | **loss / RTT** (RR, REMB) |
|---|---|---|
| Granularity | per-packet arrival deltas | per-report-window aggregate |
| Resolution | 250 µs delta ticks, 64 ms reference base | fraction-lost over a window (8-bit) |
| Signalling cost | one RTCP packet per feedback round | one RR per report interval |
| Needs negotiation | **yes** — `a=rtcp-fb:205 transport-cc` | no (baseline RTCP) |
| Latency | low (arrival times, not window statistics) | high (a window must fill before it means anything) |
| Accuracy | high under reordering (per-packet) | degrades with reordering (counts reordering as loss) |
| Browser support | Chrome / Edge / Firefox: yes | universal |
| Loss information | implicit (not-received status symbols) | explicit |

Two properties of transport-cc make it the preferred signal when it is
available:

1. **It survives reordering.** A packet that arrives late is still a packet
   that arrived; transport-cc reports per-packet arrival, so a reordered
   packet does not look like a lost one. A fraction-lost counter over a window
   cannot make that distinction.
2. **It is a delay signal, not just a loss signal.** transport-cc's arrival
   deltas reveal *queueing delay* — the delay a packet accumulates in a
   bottleneck buffer — which is the earliest signal of congestion available.
   Loss is the late one.

But transport-cc has a hard prerequisite: **the peer must have advertised
support for it.** A sender that emits transport-cc feedback to a peer that
did not negotiate it is sending a packet the peer will discard. And a sender
that *receives* transport-cc from a peer that did offer it, but has no
negotiation state, cannot know whether the feedback is authoritative.

This is why the policy is a **policy** and not a hard-coded preference: the
signal-selection decision depends on a fact that only the SDP negotiation
knows.

### The wire formats, and a correction

While preparing M1, the transport-cc reference-time semantics had to be
pinned down, because the arrival-time series that F2 consumes is computed
from it. The repository's comment on the parsed field was wrong
(`src/modules/rtp/src/parser.cpp`, and the field name in
`src/modules/rtp/include/nimrtc/rtp/packet.hpp`). Three sources agree on the
truth:

* **`draft-holmer-rmcat-transport-wide-cc-extensions-01`** §3.1: the
  reference time is "a 24-bit[s] … absolute reference time … **The value is
  to be interpreted in multiples of 64ms**", and each receive delta is
  expressed in multiples of 250 µs.
* **libwebrtc** (`modules/rtp_rtcp/source/rtcp_packet/transport_feedback.h`):
  `kDeltaTick = TimeDelta::Micros(250)` and, in the `.cc`,
  `kBaseTimeTick = kDeltaTick * (1 << 8)` — i.e. 250 µs × 256 = **64 ms**,
  matching the draft.
* **Pion** (`pion/rtcp`, `transport_layer_cc.go`): `TypeTCCDeltaScaleFactor
  = 250` for the receive delta.

The existing comment cited "RFC 8888 §7.1" and a 1 kHz tick. RFC 8888 is the
*Common Control Feedback Format* (CCFB, FMT=11) — a different feedback
format from transport-cc (FMT=15) — and the unit is 64 ms, not 1 ms. The
**delta** unit in the repository (250 µs) was already correct. The reference
time was wrong by a factor of 64.

A second correction came out of the same exercise, and it is larger than
the reference-time one: the **packet-chunk and receive-delta encoding** in
the repository was not the draft's encoding at all. See "The chunk and delta
encoding" below.

This ADR records the corrected semantics as the normative statement for
NimRTC, and M1's implementation follows it.

## The chunk and delta encoding

Three independent sources agree on the packet-chunk and receive-delta
encoding, and the implementation in `src/modules/rtp/src/parser.cpp` was
wrong in four separate ways before this ADR's implementation notes were
carried out.

**The chunk header *contains* the status symbols.** A chunk is 16 bits:

| Chunk | Layout |
|---|---|
| Run length | `0` \| symbol (2 bit) \| run length (13 bit) |
| Status vector, 1-bit symbols | `1` \| `0` \| 14 symbol bits (symbol *i* at bit 13−*i*) |
| Status vector, 2-bit symbols | `1` \| `1` \| 7 symbols × 2 bit (symbol *i* at bits 13−2*i* .. 12−2*i*) |

There is **no symbol-count field**: the count is implied by the `S` bit
(14 symbols or 7 symbols). The previous implementation treated the low 14
bits as a count and read the symbols from the bytes *after* the chunk
header, which is not where they are.

**The receive delta is variable-width, and its width is the symbol's.**
The deltas follow *all* the chunks, ordered by packet:

| Symbol | Meaning | Delta on the wire |
|---|---|---|
| `0` | not received | none |
| `1` | received, small delta | **8-bit unsigned** (0 … 255 ticks of 250 µs) |
| `2` | received, large or negative delta | **16-bit signed** (−32768 … 32767 ticks of 250 µs) |
| `3` | received, without delta | none |

The previous implementation read a fixed **14-bit** delta per received
packet — that is the RFC 8888 (CCFB) delta encoding, not the transport-cc
one — and it read the deltas *inside* the chunk loop instead of after all
the chunks. A run-length chunk of "received" packets also carries one
delta *per packet in the run*, which the previous implementation did not
read at all.

**Symbol 3 exists.** The draft's §3.1.1 marks it "[Reserved]", but its own
Example 2 uses it — a run of 24 packets "received, w/o recv delta" — and
Pion names it `TypeTCCPacketReceivedWithoutDelta`. libwebrtc rejects a
symbol 3 as a malformed packet, but a receiver that has to interoperate
with the draft has to parse it. NimRTC parses it as "received, no delta".

**The status symbols of a 1-bit chunk are inverted in the draft's prose.**
§3.1.4 says a one-bit symbol vector contains only "packet received" (0)
and "packet not received" (1), but the draft's own Example 1 and
libwebrtc's encoder both use 0 = not received, 1 = received. NimRTC follows
the example and libwebrtc.

## Decision

**NimRTC implements one bandwidth estimator with two signal paths. The
signal-selection policy lives in a plug-in-configurable component, has an
automatic default, and resolves to `transport-cc` when — and only when — the
peer advertised it.**

### Decision 1 — One estimator, two paths

There is **one** `plugins::IBwe` implementation contract and **one**
`plugins::BweFeedback` structure. The signal a sample came from is carried
**inside** the feedback structure as an explicit discriminator, not by having
two estimator classes.

```cpp
enum class NIMRTC_API_STABLE BweSignalSource : std::uint8_t {
    kLossRtt,       // receiver report / REMB / local loss + RTT observation
    kTransportCc,   // RTPFB FMT=15 arrival-time series
    kRemb,          // receiver-reported maximum bitrate (a loss/RTT-family hint)
};
```

Rationale: a congestion-control algorithm that reacts to "10% loss over the
last window" and one that reacts to "the delay trend is rising" are different
*algorithms*, but they are not different *estimators* in the architectural
sense. Splitting them into two classes would mean two plugin registries, two
configuration surfaces, two sets of statistics, and two ways for a host to
misconfigure the system. One structure with a discriminator keeps the seam
wide enough for both and narrow enough that a host writes one plugin.

The consequence is that the estimator sees a **superset** of information and
must be told which kind it is looking at. That is the discriminator's job.

### Decision 2 — Automatic selection, with a plug-in-configurable override

The policy resolves the driving signal from **two** facts:

1. **Negotiation state**: did the peer advertise `transport-cc` for the
   relevant m-section (`a=rtcp-fb:205 transport-cc`)? This is a fact
   established by SDP offer/answer.
2. **Freshness**: is transport-cc feedback *actually arriving*? A peer that
   advertised `transport-cc` and then never sends it must not starve the
   estimator.

| Peer advertised transport-cc | transport-cc feedback arriving | Driving signal |
|---|---|---|
| yes | yes | `kTransportCc` |
| yes | no (stale beyond `transport_cc_timeout`) | fall back to `kLossRtt` |
| no | n/a | `kLossRtt` |

The **default** is the table above — automatic. The override is a
configuration value on the policy component, allowing a host to force
`kLossRtt` (for debugging, or for a peer that advertises the capability but
sends unusable feedback).

The override is **not** "force transport-cc": forcing a signal the peer never
sends would produce a silent stall, which is worse than the automatic
default. The only override direction is "prefer the loss/RTT path", plus a
"require transport-cc" mode that *fails loudly* (reports
`kSignalUnavailable`) instead of falling back silently — for a host that
wants to know its transport-cc path is broken rather than have it papered
over.

### Decision 3 — Arbitration: transport-cc wins, REMB is a hint

When both signals are present in the same feedback window:

* **transport-cc drives the estimate.** It is per-packet, delay-sensitive,
  and reorder-robust.
* **REMB is an upper bound, not a target.** REMB is the receiver's opinion
  of the *maximum* bitrate the path currently sustains. It is a hint about
  capacity; it is not an observation of the current state. The existing
  `kRembTrustFactor = 0.8` semantics in `src/modules/bwe/` are **preserved
  unchanged** (F7's acceptance criterion): REMB may raise the estimate, at
  80 % of the reported value, and may never lower it below the
  transport-cc-driven value.
* **Loss/RTT RTT and loss fill in when transport-cc has no data.** Between
  two transport-cc feedback packets there is a gap. During that gap, the
  estimator is not blind: the RR-derived loss and RTT are fed in as a
  **loss/RTT** sample, tagged as such.

The arbitration rule, stated once:

> The most recent sample wins, **except** that a REMB sample may only
> *raise* the estimate, and a loss/RTT sample may not override a
> transport-cc sample that is newer than it.

### Decision 4 — What the transport-cc path actually measures

The transport-cc path produces a **per-packet arrival-time series** for the
media packets of one SSRC. From it, the following are derived:

* **arrival time** of each reported packet, reconstructed as
  `reference_time + Σ(deltas)`, with the reference time in **64 ms** units
  and each delta in **250 µs** units (Decision: the corrected semantics
  above). The reconstruction is *relative*: the reference time is an
  arbitrary sender-chosen base, so only differences are meaningful, and the
  series is used for **trends**, not for absolute wall-clock.* **receive rate** (packets per second → bits per second), over a sliding
  window.
* **delay trend**: the change in the inter-arrival spacing relative to the
  send spacing. A rising trend is the congestion signal — it is the one
  transport-cc offers that loss-based feedback cannot.
* **loss**: packets reported as "not received" within the feedback window.

The transport-cc path is **not** allowed to drive the estimate from a single
sample. It needs a window of packets before its receive rate is meaningful;
before that, the estimator falls back to the loss/RTT path. This is why the
policy has a *freshness* axis and not just an *advertised* axis.

### Decision 5 — Where the policy lives

The policy is a **component with a seam**, not engine logic:

* The **signal-selection decision** is a plug-in-visible, configurable
  component (`src/plugins/` interface; implementation in
  `src/modules/net_feedback/`), so a host can inspect and override it.
* The **engine** (L4 — orchestration only) asks the component which signal
  is driving, and feeds the estimator accordingly. The engine contains no
  policy branches.
* The **estimator** (`plugins::IBwe`) receives samples with their source
  attached and applies the estimator's own algorithm. It does not know which
  signal the *peer* negotiated.

### Decision 6 — Negotiation

`transport-cc` support is negotiated per m-section via the standard RTCP
feedback attribute (`a=rtcp-fb:205 transport-cc`), RFC 4585 §4.1. The
offer carries it when the host has a transport-cc-capable estimator
installed; the answer echoes it only if the peer offered it (RFC 3264 §6.1
symmetry for this attribute).

The **negotiation-only slice** of the codec/negotiation work lands in M2,
not M1 — M1 builds the parsing and the aggregation, M2 turns on the
negotiation and the automatic selection. This keeps M1's "ends when" criterion
(a lossy channel moves the estimate) free of a dependency on SDP changes.

## Alternatives considered

### A. Two separate BWE implementations (one for transport-cc, one for loss/RTT)

**Rejected.** Two implementations means two plugin ids, two configuration
surfaces, two statistic sets, and a host that must pick the right one *at
setup time* — before it knows what the peer supports. It also makes the
"peer advertised transport-cc but sends none" failure mode a startup-time
decision instead of a runtime one. One estimator with a discriminator
handles that case as a data condition.

### B. Always use transport-cc when the peer advertises it, never fall back

**Rejected.** A peer that advertises the capability and then sends no
transport-cc feedback (a buggy peer, a middlebox, a version skew) would
stall the estimator silently. The fallback is what makes the automatic
default safe.

### C. Always use loss/RTT, treat transport-cc as an optimisation

**Rejected.** transport-cc is not merely a better loss metric — its delay
trend is a fundamentally earlier congestion signal. Using loss/RTT as the
primary would keep the existing behaviour (late detection, loss-induced
false positives under reordering) and make the transport-cc support
pointless.

### D. Make the signal choice a per-peer compile-time constant

**Rejected.** The peer set changes at runtime (a reconnect can land on a
different peer), and the negotiated capability changes with the session.
A runtime policy that can be overridden per session is the only thing that
matches how the system is actually used.

### E. Implement transport-cc parsing in the engine directly

**Rejected** (L4). The engine would then contain wire-format logic that a
host could not replace. The parsing already exists in the RTP module
(`parse_transport_cc`); M1 wires it in through the module boundary, and the
aggregation lives in `net_feedback`.

## Consequences

### Positive

* The BWE estimate becomes a function of the network instead of a constant —
  this is the change that makes the whole video plan worth doing.
* One plugin surface for the estimator, one for the policy, one for the
  feedback collector. A host that wants a custom estimator writes one
  plugin.
* The negotiated capability becomes observable, so a host can log "we are
  running on transport-cc" or "the peer did not offer it, we are on
  loss/RTT" as a first-class fact rather than inferring it.

### Negative / obligations

* `plugins::BweFeedback` grows (F3). It is an **additive** change to a
  `NIMRTC_API_STABLE` structure — new fields with defaults, plus the
  discriminator. Per ADR-015 Decision 2 this does not require a MAJOR bump;
  existing implementations of `plugins::IBwe` continue to compile and behave
  identically because the new fields default to "no transport-cc data" and
  "loss/RTT source".
* The transport-cc path needs a window of packets before it can drive the
  estimate, so during the first window after a session starts, the estimate
  is driven by the loss/RTT path. This is a real latency cost at session
  start and it is deliberate.
* An estimator that ignores the discriminator and treats every sample as
  loss/RTT will work, but it will under-use transport-cc. The seam documents
  this; the plugin-author guide (G6) must say it plainly.

### Neutral / follow-ups

* The policy component's configuration is a **new public surface** and needs
  an ADR-015 stability marker and a `tests/test_api_classification.cpp`
  baseline update (G7).
* Per-stream vs. session-wide BWE re-targeting (F8) is **not** decided here.
  It is a separate question (how a session-level estimate is divided among
  streams) and gets its own rule in the rate-control track (B2).

## Implementation notes

* **Reference-time semantics** (normative, correcting the previous
  comment): reference time is in multiples of **64 ms**; each receive delta
  is in multiples of **250 µs**. The absolute reference time is an arbitrary
  sender-chosen base — only differences between reference times are
  meaningful, and the series is used for trends.
* **Chunk and delta semantics** (normative, correcting the previous
  implementation): the chunk header contains the status symbols — a run
  length chunk is `0 | symbol (2 bit) | run length (13 bit)`, a status
  vector chunk is `1 | S | 14 symbol bits`, giving 14 one-bit symbols
  (S=0) or 7 two-bit symbols (S=1), with no symbol-count field. The
  receive deltas follow all the chunks, one per received-with-delta packet,
  and their width is the symbol's: 8-bit unsigned for symbol 1, 16-bit
  signed for symbol 2, none for symbols 0 and 3. A run-length chunk of
  "received" packets carries one delta per packet in the run.
* **Symbol 3** is parsed as "received, without delta". The draft marks it
  reserved but uses it in its own Example 2, and Pion names it
  `TypeTCCPacketReceivedWithoutDelta`; libwebrtc rejects it, so NimRTC's
  parser is deliberately more permissive than Chrome's.
* **One-bit symbols are 0 = not received, 1 = received.** The draft's §3.1.4
  prose has this inverted; the draft's Example 1 and libwebrtc agree on the
  non-inverted reading, and so does NimRTC.
* **`PacketStatus::ReceivedNoDelta = 3`** was added to the
  `NIMRTC_API_STABLE` enum. Adding an enumerator is additive and does not
  change the enum's size, so it is not an ABI break. The pre-existing
  enumerator names (`Received` = 1, `ReceivedSmall` = 2) are *inverted*
  relative to the wire semantics and cannot be renamed without a MAJOR
  bump, so the header documents them explicitly and callers should use the
  numeric value.
* **`TransportCcFeedback` gained two fields** (`reference_time_ms`,
  `feedback_packet_count`) and `TransportCcPacketStatus` gained none —
  the delta width is derivable from the status symbol, so a separate
  `delta_bytes` field would have been redundant. Per ADR-015 Decision 4,
  ABI is committed only within the v1.0.x patch line, so a MINOR bump
  (v1.1.0 → v1.2.0) may change the layout of a public structure.
* **RTCP padding is honoured.** The padding octets trail the message and
  are excluded from both the chunk section and the receive-delta list;
  otherwise the padding bytes would be parsed as a packet chunk.
* **`kRembTrustFactor = 0.8` is unchanged.** F7 keeps the existing
  REMB-override semantics; REMB becomes one *input* rather than the only
  receiver-side opinion.
* The `net_feedback` module is named to avoid confusion with the existing
  `video_rtcp_feedback` module, which handles *video-side* PSFB / RTPFB
  feedback (NACK / PLI / FIR), not sender-side network feedback.

## Status

**Accepted (landed).** M1's parsing and aggregation have landed, and so
has the policy's table-driven test (peer supports transport-cc →
`kTransportCc`; peer does not → `kLossRtt`; both paths feed the same
`IBwe`).

What landed, in order:

* The transport-cc **parsing** half of M1: the corrections above are
  implemented in `src/modules/rtp/src/parser.cpp` and covered by
  `src/modules/rtp/tests/test_transport_cc.cpp` (15 cases, one per
  wire-format shape the draft defines, plus the truncation and type-error
  paths).
* The **aggregation** half (F2's `net_feedback` module): the inbound RTCP
  compound datagram is demultiplexed, the receiver-side signals are
  collected behind the `plugins::INetFeedback` seam, and the engine hands
  the aggregated snapshot to `IBwe::on_feedback()` once per feedback
  report — `src/modules/net_feedback/tests/test_net_feedback.cpp`.
* The **policy** (F5/F7): `plugins::NetFeedbackPolicy`
  (`kAuto` / `kLossRtt` / `kRequireTransportCc`) plus
  `plugins::NetFeedbackConfig`, selected by
  `EngineConfig::net_feedback_signal`, driven by the negotiated
  `a=rtcp-fb` set. The table-driven test covers the negotiation
  resolution, the stale-signal fallback and the explicit override.
* Decision 4's **delay trend**: the transport-cc arrival series is turned
  into an inter-arrival-spacing trend inside the estimator
  (`src/modules/bwe/src/arrival_trend.hpp`), so a rising spacing —
  congestion before it becomes loss — drives the same multiplicative
  decrease a loss event does. Covered by
  `src/modules/bwe/tests/test_bwe.cpp` and an end-to-end case in
  `tests/test_engine_net_feedback.cpp`.

This ADR's **Status** is therefore **Accepted (landed)**, and
`docs/adr/README.md` lists it in the current-ADR table.
