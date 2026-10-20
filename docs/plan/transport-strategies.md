# Transport Strategies for Use Cases

| | |
|---|---|
| Version | v0.12.0-RFC (review draft) |
| Date | 2026-10-10 |
| Status | **Draft** for v0.12.0 — companion to `transport-selection.md` |
| Binds | v0.12.0 (T-PAL Slice 7 / 7.6 / 8 + Profile schema v1.1) |
| Refs    | `docs/plan/transport-selection.md` §5.3 (CapabilitySelector rules); `docs/plan/v0.11-plan.md` §2.6 (Profile table); `profiles/schema/profile-v1.1.json` (additive `transport` section); ADR-010 (Profile JSON first-class) |

> This document names the **five canonical use-case strategies** that map
> a Profile's declared intent to a concrete (media_stack, control_stack)
> pair through `CapabilitySelector`. The Selector rule engine already
> implements the priorities in `transport-selection.md` §5.3; this doc
> gives each rule a **name** and a **default policy** so application code,
> Profile JSONs, and the registry can refer to a strategy by id instead
> of reasoning about boolean flags at every call site.

---

## 1. Motivation

`transport-selection.md` §5.3 enumerates four Selector priority rules in
terms of two boolean flags (`needs_browser_interop`, `needs_high_freq_control`)
and one hint (`prefer_quic`). At every Profile load site, application code
must (a) read those flags from the Profile JSON, (b) decide what they
should be for the use case at hand, and (c) hand them to the Selector.
Step (b) is repetitive: every robot-teleop deployment asks the same
questions, every browser-call asks the same questions, every SFU asks
the same questions.

**The five named strategies** below are the canonical answers to step (b).
Each strategy is a small C++ struct (and a JSON-serialisable
counterpart) that pre-fills `TransportRequirements` from a use-case name.
Application code becomes:

```cpp
// Before
TransportRequirements req;
req.needs_browser_interop = false;
req.needs_high_freq_control = true;
req.max_p99_ms = 50;
auto session = selector_->select(req);

// After
auto session = selector_->select(strategy_for("teleop-ultra-low-latency"));
```

Same behaviour, no repeated flag-filling, and the strategy name appears
in log lines and metrics so on-call engineers can grep by strategy.

---

## 2. The five named strategies

| ID | Display name | Browser interop | High-freq control | p99 budget (ms) | Media stack | Control stack |
|---|---|---|---|---|---|---|
| `browser-call` | Browser call / video conference | **required** | optional | 200 | webrtc-classic | webrtc-classic (DC) or raw-udp-arq (opt-in) |
| `teleop-ultra-low-latency` | Robot / drone teleop, control loop dominant | not required | **required** | **50** | raw-udp-arq | raw-udp-arq |
| `agent-gateway` | Server-side AI Agent media gateway | not required | **required** | 100 | webrtc-classic | raw-udp-arq |
| `sfu-relay` | SFU forwarding middlebox | not required | not required | 200 | webrtc-classic | null |
| `cross-network-peer` | Cross-network peer, lossy / mobile | optional | optional | 150 | webrtc-classic | webrtc-classic (DC) |

The strategies cover the four Selector rules plus one new combination
(`cross-network-peer` = "browser interop optional, no high-freq control,
p99 budget 150 ms") that was implicit in §5.3 but never named.

### 2.1 Strategy `browser-call`

**Use case**: 1:1 video/voice call terminating in Chrome / Firefox /
Safari. Media plane must be webrtc-classic for browser interop; control
plane is the in-band DC plane unless the application explicitly asks
for raw-udp-arq (see variant below).

```cpp
struct BrowserCallStrategy {
    bool needs_browser_interop   = true;       // §5.3 rule 1
    bool needs_high_freq_control = false;
    std::optional<int> max_p99_ms = 200;
    bool prefer_quic             = false;
};
// Resulting TransportSession:
//   media_stack   = webrtc-classic
//   control_stack = nullptr  (control rides the media stack's DC)
```

**JSON counterpart** (Profile schema v1.1 additive `transport` section):

```jsonc
{
  "transport": {
    "strategy": "browser-call",
    "media":   { "stack": "webrtc-classic", "max_p99_ms": 200 },
    "control": { "enabled": false }
  }
}
```

**Variant — `browser-call + raw-udp-arq`**: When the application declares
a tight p99 budget on the control plane (e.g. in-meeting whiteboard
strokes at 60 Hz) it may opt into the side-stack:

```jsonc
{
  "transport": {
    "strategy": "browser-call",
    "media":   { "stack": "webrtc-classic", "max_p99_ms": 200 },
    "control": {
      "stack":         "raw-udp-arq",
      "max_p99_ms":    80,
      "freq_hz":       60,
      "reliability":   "partial_reliable",
      "ttl_ms":        50
    }
  }
}
```

Maps to: `media_stack = webrtc-classic`, `control_stack = raw-udp-arq`
(§5.3 rule 1 with high-freq control enabled).

### 2.2 Strategy `teleop-ultra-low-latency`

**Use case**: Robot / drone / vehicle teleoperation. The dominant
constraint is the **closed-loop control round trip**: every extra ms
in the control channel directly degrades the operator's feel. Media
(video from the robot's camera) is present but secondary — operator
will tolerate slightly higher video latency if the control channel is
snappy.

```cpp
struct TeleopUltraLowLatencyStrategy {
    bool needs_browser_interop   = false;      // §5.3 rule 2
    bool needs_high_freq_control = true;
    std::optional<int> max_p99_ms = 50;        // tightest of any strategy
    bool prefer_quic             = false;
};
// Resulting TransportSession:
//   media_stack   = raw-udp-arq  (raw UDP, no SCTP tax)
//   control_stack = raw-udp-arq  (independent stack — own ICE selected-pair)
```

**JSON counterpart**:

```jsonc
{
  "transport": {
    "strategy": "teleop-ultra-low-latency",
    "media": {
      "stack":      "raw-udp-arq",
      "max_p99_ms": 100
    },
    "control": {
      "stack":       "raw-udp-arq",
      "max_p99_ms":  50,
      "freq_hz":     200,
      "reliability": "reliable_ordered",
      "ttl_ms":      100
    }
  }
}
```

**Why two raw-udp-arq stacks instead of one?** The §5.3 rule 2
prescription is two independent stacks — each stack carries its own
ICE selected-pair, its own DTLS-PSK session, its own congestion
window. The two stacks do not interfere even when the media path
encounters loss bursts. This is the only strategy where media and
control both deliberately avoid SCTP and browser-classic.

**Variant — LAN teleop**: When both peers are on the same LAN (e.g.
a factory-floor deployment), set `max_p99_ms` to 20 ms on the control
plane. Latency floor becomes the OS scheduling jitter + ARQ RTO base
(default 50 ms; the v0.12.x benchmark will measure this; see §6).

### 2.3 Strategy `agent-gateway`

**Use case**: Server-side AI Agent media gateway. One side of the
gateway is a browser peer (so browser interop may be required); the
other side is the LLM/ASR pipeline (which never sees a browser).
The data plane is the dominant traffic: model commands, partial
ASR results, function-call returns all ride on a high-frequency
control channel. Media is the secondary traffic (operator's voice
into the model, model's TTS back to the operator).

```cpp
struct AgentGatewayStrategy {
    bool needs_browser_interop   = true;       // operator side is a browser
    bool needs_high_freq_control = true;       // LLM ↔ engine traffic
    std::optional<int> max_p99_ms = 100;
    bool prefer_quic             = false;
};
// Resulting TransportSession:
//   media_stack   = webrtc-classic  (browser interop)
//   control_stack = raw-udp-arq     (LLM/ASR traffic, p99 ≤ 80 ms)
```

**JSON counterpart**:

```jsonc
{
  "transport": {
    "strategy": "agent-gateway",
    "media": {
      "stack":      "webrtc-classic",
      "max_p99_ms": 200
    },
    "control": {
      "stack":       "raw-udp-arq",
      "max_p99_ms":  80,
      "freq_hz":     100,
      "reliability": "reliable_ordered"
    }
  }
}
```

Maps to: §5.3 rule 1b (browser interop + high-freq control).

### 2.4 Strategy `sfu-relay`

**Use case**: SFU middlebox. Receives N inbound media streams, forwards
them to M outbound peers. No L2 media processing. The "control plane"
is the SFU's own internal coordination channel, not user data — so no
control stack is required.

```cpp
struct SfuRelayStrategy {
    bool needs_browser_interop   = true;       // peers are browsers
    bool needs_high_freq_control = false;
    std::optional<int> max_p99_ms = 200;
    bool prefer_quic             = false;
};
// Resulting TransportSession:
//   media_stack   = webrtc-classic
//   control_stack = nullptr  (control rides the media stack's DC)
```

**JSON counterpart**:

```jsonc
{
  "transport": {
    "strategy": "sfu-relay",
    "media": {
      "stack":      "webrtc-classic",
      "max_p99_ms": 200
    },
    "control": { "enabled": false }
  }
}
```

**Variant — SFU with DC forwarding policy**: When the SFU must
forward DataChannels across the hop (DC-3 in v0.11-plan §2.2), enable
the control stack with `stack: "webrtc-classic"` (i.e. forward via
the media stack's DC). This was the v0.12.0 default; the pre-1.0
DC forwarding decision (§7) is now **closed as NO** (BDFL,
2026-10-19 — SFU is not a project core goal; see
`v1.0-plan.md` §7 + §12.5), so the SFU-with-DC-forwarding
variant is **not on any roadmap**. The `relay` strategy in
`relay-agent` Profile (single-strategy relay, no DC forwarding)
remains the canonical relay use case.

### 2.5 Strategy `cross-network-peer`

**Use case**: Peer-to-peer call where one or both peers are on a
lossy / mobile / cross-ISP link. The dominant concern is **robustness**,
not raw latency: the strategy prefers a webrtc-classic media plane
(which has QUIC-style congestion control when the DTLS path uses
Goog-CC — see v0.11-plan §2.2) over a raw-udp-arq plane (which has
only the ARQ skeleton, no CC yet — see transport-selection.md §3).

```cpp
struct CrossNetworkPeerStrategy {
    bool needs_browser_interop   = true;       // default true; opt out for native peers
    bool needs_high_freq_control = false;
    std::optional<int> max_p99_ms = 150;       // tighter than browser-call
    bool prefer_quic             = false;
};
// Resulting TransportSession:
//   media_stack   = webrtc-classic
//   control_stack = nullptr  (DC plane only)
```

**JSON counterpart**:

```jsonc
{
  "transport": {
    "strategy": "cross-network-peer",
    "media":   { "stack": "webrtc-classic", "max_p99_ms": 150 },
    "control": { "enabled": false }
  }
}
```

**When to use this over `browser-call`**: the same `webrtc-classic`
media stack, but with a tighter `max_p99_ms` budget (150 vs 200 ms).
The application supplies the budget; the Selector does not enforce it
— it just records it in the `TransportSession` so monitoring / e2e
tests can assert against it.

---

## 3. Mapping to existing Profile JSONs (v1.1 additive)

The v1.0 Profile schema (ADR-010) does not have a `transport` section.
The v1.1 schema (this release) adds it as an **additive** top-level
key — see `profiles/schema/profile-v1.1.json` and ADR-010 §4
(versioning). Existing v1.0 profiles continue to load; the loader
defaults the `transport` section to `null` (which means: use the
engine's compile-time default, equivalent to `browser-call`).

| Profile JSON | v1.1 default `transport.strategy` | Rationale |
|---|---|---|
| `call.json` | `browser-call` | 1:1 browser voice/video call |
| `live.json` | `browser-call` + `max_p99_ms: 300` | 1:N broadcast; latency budget looser |
| `transport.json` | `sfu-relay` | Gateway ICE transport |
| `agent.json` | `agent-gateway` | SDK side of an Agent pipeline |
| `agent-gateway.json` | `agent-gateway` | Server side of an Agent pipeline |
| `agent-low-latency.json` | `agent-gateway` + `max_p99_ms: 80` | Tightest Agent variant |
| `teleop.json` | `teleop-ultra-low-latency` | Robot / drone teleop |
| `sfu.json` | `sfu-relay` | relay forwarding middlebox |
| `sfu-agent.json` | `sfu-relay` + `agent-gateway` (composite) | relay + Agent hybrid; uses `sfu-relay` as the primary strategy, with `agent-gateway` overriding the control plane |

The composite `sfu-agent` case is resolved by the engine picking
`sfu-relay` for the relay side and `agent-gateway` for the Agent side
of the same engine; the `TransportSession` is built twice. See
`docs/zh/architecture.md` §2.6 for the relay+Agent hybrid description.

---

## 4. Selector rule audit

| Rule (transport-selection.md §5.3) | Strategies that hit it | Selector trace |
|---|---|---|
| 1. Browser interop required, no high-freq control | `browser-call`, `sfu-relay`, `cross-network-peer` | `media=webrtc-classic, control=null` |
| 1b. Browser interop required + high-freq control | `agent-gateway` | `media=webrtc-classic, control=raw-udp-arq` |
| 2. No browser interop + high-freq control | `teleop-ultra-low-latency` | `media=raw-udp-arq, control=raw-udp-arq` |
| 3. No browser interop + prefer_quic | (none of the 5) | `media=webrtc-quic, control=…` — future strategy `webrtc-quic-call` lands in v1.x with WebTransport |
| 4. Default | (none — every strategy declares its rule explicitly) | `media=webrtc-classic, control=null` |

The strategies are **a strict superset** of §5.3's four rules. Strategies
1–3 map to one of §5.3's rules; strategy 4 (cross-network-peer) is a
new combination that §5.3 does not enumerate but is consistent with it
(§5.3 rule 1 with a tighter p99 cap).

---

## 5. C++ surface (preview)

The five strategies are exposed as compile-time constants in
`src/transport/include/nimrtc/transport/transport_strategy.hpp` (added
in PR3 — `feat/raw-udp-arq-stack`):

```cpp
namespace nimrtc {

inline constexpr TransportRequirements kStrategyBrowserCall{
    .needs_browser_interop   = true,
    .needs_high_freq_control = false,
    .max_p99_ms              = 200,
};

inline constexpr TransportRequirements kStrategyTeleopUltraLowLatency{
    .needs_browser_interop   = false,
    .needs_high_freq_control = true,
    .max_p99_ms              = 50,
};

inline constexpr TransportRequirements kStrategyAgentGateway{
    .needs_browser_interop   = true,
    .needs_high_freq_control = true,
    .max_p99_ms              = 100,
};

inline constexpr TransportRequirements kStrategySfuRelay{
    .needs_browser_interop   = true,
    .needs_high_freq_control = false,
    .max_p99_ms              = 200,
};

inline constexpr TransportRequirements kStrategyCrossNetworkPeer{
    .needs_browser_interop   = true,
    .needs_high_freq_control = false,
    .max_p99_ms              = 150,
};

// Lookup by name (used by the Profile loader).
const TransportRequirements& strategy_by_name(std::string_view name);

} // namespace nimrtc
```

The `strategy_by_name` lookup is wired into the v1.1 Profile loader
(see `src/modules/assembly/profiles.cpp` post-PR2). An unknown
strategy name throws `std::invalid_argument` with a list of the five
valid ids.

---

## 6. Open questions

1. **`StrategyCrossNetworkPeer` naming** — ~~Decision deferred to PR2
   (Profile v1.1 review).~~ **BDFL decision (2026-10-13): keep
   `cross-network-peer`.** See §8 review checklist for rationale.
2. **Composite `sfu-agent` resolution** — ~~deferred to pre-1.0
   decision window (v0.11-plan §7).~~ **BDFL decision (2026-10-13):
   out of v0.12.0 scope; v0.12.0 collapses to the higher-priority
   single strategy with a `WARN` log.** **Update (2026-10-19):**
   the pre-1.0 window is now closed as `v1.0-plan.md` §7 = NO
   (BDFL: SFU is not a project core goal). The composite
   `sfu-agent` resolution is **not on any roadmap**; if the
   SFU track is reopened in a future cycle, a new
   `v1.1.0-plan.md` re-evaluates the design (out of scope
   for v1.0.0).
3. **Strategy-by-Profile vs Profile-by-strategy** — the current doc
   assumes Profile-led (`transport.strategy = "agent-gateway"`),
   matching the v1.0 schema. Pre-1.0 review (v0.11-plan §7) may
   revisit; for v0.12.0 the Profile-led model is the only contract
   and `test_transport_strategy.ProfileV1RoundTrip` enforces it.
4. **`max_p99_ms` enforcement** — the Selector currently **records**
   the budget but does not enforce it. A v0.13+ cycle may add a
   `StrategyValidator` that fails the engine `open()` if the chosen
   stack's measured p99 exceeds the budget. Out of v0.12.0 scope;
   recorded here so v0.13 release notes can pick it up.

---

## 7. References

- `docs/plan/transport-selection.md` §3 (latency numbers), §4 (candidate
  comparison), §5.2 (Stack/Selector interfaces), §5.3 (Selector
  priority rules)
- `docs/plan/v0.11-plan.md` §2.6 (Profile table), §9 (Slice tracker),
  §10 (v1.x WebTransport direction)
- `docs/plan/pal-architecture.md` §3.5 (PAL Slice 1/2/3 boundary)
- `docs/adr/ADR-010-profile-json-format.md` (Profile JSON first-class)
- `src/transport/include/nimrtc/transport/transport_selector.hpp`
  (CapabilitySelector C++ surface)
- `src/raw_udp/include/nimrtc/raw_udp/raw_udp_datagram.hpp`
  (IRawUdpDatagram Slice 6 surface)
- `src/transport/src/transport_plugin.cpp` (registration entry points)

---

## 8. Review checklist

- [x] Five strategies named, each maps to one §5.3 Selector rule
- [x] Each strategy has a JSON counterpart under v1.1 schema
- [x] Each strategy has a C++ constant preview
- [x] §4 audit confirms no §5.3 rule is missed
- [x] §3 mapping covers all 9 existing Profile JSONs
- [x] §6 captures open questions without leaving dark pitfalls
- [x] No non-ASCII in this document (per `tools/check_head_non_ascii.py`)
- [x] No conflicts with `transport-selection.md` §5.3 wording
- [x] No conflicts with v0.11-plan §2.6 Profile table
- [x] **BDFL review (2026-10-13): §6 #1 — keep `cross-network-peer`**
      as the canonical id. The JSON key and the `kStrategyCrossNetworkPeer`
      C++ constant stay. Reason: the v0.11-plan §2.6 profile table already
      uses "cross-network-peer" in human-readable form; renaming would
      break docs. `robust-peer` is too vague; `lossy-link` is a property
      not a use case; `mobile-friendly` overstates the scope (it covers
      any NAT-traversal / lossy-link scenario, not only mobile).
- [x] **BDFL review (2026-10-13; reaffirmed 2026-10-19): §6 #2 —
      composite `sfu-agent` is out of v0.12.0 scope and is
      **no longer on any roadmap**.** Defer the multi-`TransportSession`
      engine decision to the pre-1.0 window (v0.11-plan §7). The
      pre-1.0 window is now closed as `v1.0-plan.md` §7 = NO
      (BDFL, 2026-10-19: SFU is not a project core goal), so
      the composite sfu-agent resolution is **not on any roadmap**.
      For
      v0.12.0, JSON Profiles that name a composite should fall back
      to the higher-priority single-strategy rule (§5.3 priority
      list, with SFU outranking agent), and the loader should log
      a `WARN: composite profile collapsed to <strategy>`. This
      matches the v0.11.0 behavior of falling back to a single
      transport slot without surfacing an error to the caller.
- [x] **BDFL review (2026-10-13): §5 — `strategy_by_name` lives
      in `src/transport/include/nimrtc/transport/transport_strategy.hpp`**
      (current plan). Reason: the five `kStrategy*` constants are
      the same translation unit; `assembly::profiles.cpp` is the
      caller and only needs the symbol, not its definition. The
      `assembly` module is a higher-level layer; moving the lookup
      there would create a cycle (transport depends on no module
      above PAL Slice 2, assembly depends on transport).
- [x] **BDFL review (2026-10-13): PR2 — v1.0→v1.1 Profile migration
      is purely additive.** Confirmed against
      `profiles/schema/profile-v1.1.json`: the new top-level
      `transport` object is optional; when present, the `strategy`
      field is the only required sub-field, and all other v1.1
      fields (`requirements`, `stack_hints`, `fallback_chain`) are
      optional. v1.0 fields are untouched: same names, same types,
      same semantics. No field removed, no field renamed. The
      `test_transport_strategy.ProfileV1RoundTrip` and
      `ProfileInputBridge.*` subtests assert byte-level backward
      compatibility for all 9 existing v1.0 JSONs.
