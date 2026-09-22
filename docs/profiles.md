# NimRTC Profile Library

Single-page index of every Profile shipped with NimRTC. Profiles are the
**canonical, JSON-serialisable** assembly unit (see ADR-010 +
[`assembly.hpp`](../src/modules/assembly/include/nimrtc/assembly/assembly.hpp)),
designed to be safely round-tripped across processes, persisted to disk,
and diff-reviewed in pull requests.

## Profile matrix (9 profiles)

| name | use case | jb | audio3a | codec | bwe | datachannel | distinguishing feature |
|------|----------|----|---------|-------|-----|-------------|------------------------|
| `call` | 1:1 bidirectional voice call | adaptive / low_latency / 40 ms | webrtc_apm | opus | aimd | no | P1 default; balanced latency ↔ quality |
| `live` | live broadcast (one-way server push) | adaptive / live / 80 ms | webrtc_apm | opus | aimd (2/20 Mbps) | no | weighted_fair scheduler; 2 Mbps initial BWE |
| `transport` | embedded ICE transport gateway | none | none | none | none | no | pure SRTP passthrough; no L2 processing |
| `agent` | AI Agent SDK (1:1 voice/text) | adaptive / low_latency / 30 ms | webrtc_apm | opus | aimd (.5/5 Mbps) | yes | timeline + DC for Agent observability |
| `teleop` | robot teleoperation | adaptive / low_latency / 20 ms | none | opus | aimd (2/20 Mbps) | yes | control_weight=20 (twice call's 10); strict-priority |
| `sfu` | server-side SFU forwarding | none | none | none | none | no | `sfu_enabled` marker; `max_sessions_per_engine`=32 |
| `sfu-agent` | SFU + AI Agent hybrid (N:1) | none (relay) | webrtc_apm (Agent side) | none (relay) | none (relay) | yes | `sfu_enabled`+`agent_gateway_enabled` markers; timeline re-calibration on egress |
| `agent-gateway` | server-side Agent media gateway | adaptive / low_latency / 30 ms | webrtc_apm (tap surface) | opus | aimd (.5/5 Mbps) | yes | exposes pre/post-3A PCM tap for ASR / wake-word / VAD |
| `agent-low-latency` | Agent SDK's lowest-latency variant | adaptive / low_latency / 20 ms | webrtc_apm (AGC off) | opus (FEC off) | aimd (.5/5 Mbps) | yes | `agent_low_latency` marker; AGC level estimator off; Opus FEC off |

### When to pick each profile

**`call`** — Default for any 1:1 voice call (VoIP, customer support, voice
chat). Balanced latency / quality trade-offs.

**`live`** — Server-to-many broadcast (CDN push, webinar, livestream).
Higher buffer depth to absorb burst re-ordering. Use weighted_fair because
audio and video share the link.

**`transport`** — Embedded gateway / protocol translator / test harness
where the engine is only there to terminate DTLS-SRTP and forward packets.
Skips all media processing.

**`agent`** — AI Agent SDK's standard profile for 1:1 voice/text
interaction. Enables DataChannel (for control / state / partials) and
timeline (for mouth-to-ASR latency tracking).

**`teleop`** — Remote-controlled robot / drone / vehicle. Strict-priority
scheduler with control_weight=20 ensures control commands preempt
everything else. 20 ms JB initial delay is the tightest v1.0 option.

**`sfu`** — In-process SFU middlebox that forwards SRTP between
participants without decoding media. `sfu_enabled=true` is the marker; the
loader (per ADR-010) treats it as loader-agnostic metadata. Pair with the
SFU transport-stack plugin via `core::PluginRegistry::get_transport_stack`.

**`sfu-agent`** — SFU + AI Agent hybrid. The relay direction skips L2
processing; the Agent side keeps WebRTC APM so the Agent pipeline can
subscribe to cleaned PCM via the post-3A tap. Timeline re-calibration on
egress so downstream consumers see a stable NTP↔RTP-TS mapping across the
hop (see `docs/zh/architecture.md` §7.1).

**`agent-gateway`** — Server-side media gateway that sits between WebRTC
peers and an AI Agent pipeline. Runs WebRTC APM so the Agent can tap
cleaned (post-3A) PCM via `set_post_process_tap` and raw (pre-3A) PCM via
`set_pre_process_tap` for wake-word / VAD self-training. Audio frames are
otherwise forwarded with minimal processing.

**`agent-low-latency`** — Real-time Agent pipeline where latency is the
dominant quality dimension. Tightest JB (20 ms initial), AGC's RNN-based
level estimator off (signalled via the `agent_low_latency` marker — AGC's
variable compute cost breaks sub-frame latency tracking), Opus FEC off
(prefer application-layer retransmission).

## Schema version

- **v1.0** — `call` / `live` / `transport` / `agent` / `teleop` / `sfu`.
  Full schema lives in [ADR-010](../docs/adr/ADR-010-profile-json-format.md).
- **v1.1** (PROFILE-1, v0.11.0) — adds `sfu-agent` / `agent-gateway` /
  `agent-low-latency` and the **additive** v1.1 markers
  `sfu_enabled` / `agent_gateway_enabled` / `agent_low_latency` /
  `max_sessions_per_engine` / `audio3a_name_explicit`. Loader-agnostic —
  the C++ loader ignores them (see ADR-010 §"Schema v1.1 incremental
  fields"). Engines that care consult these markers at composition time.

## How to use

```cpp
#include <nimrtc/assembly/profiles.hpp>

// Pick a built-in profile constant and feed it to the Builder.
auto profile = nimrtc::assembly::Builder{}
    .load_profile("agent-gateway")   // ← resolved via ProfileRegistry
    .override_bwe({.impl = "aimd",
                   .params = {.initial_bitrate_bps = 750'000}})
    .build();
```

Or load a JSON file directly (round-trip with the C++ constant):

```cpp
auto profile = nimrtc::assembly::profile_from_json_file(
    "/etc/nimrtc/profiles/agent-gateway.json");
```

## JSON ↔ C++ invariant

Every profile shipped under `profiles/<name>.json` round-trips with the
corresponding `kProfile<Name>` constant in
[`profiles.hpp`](../src/modules/assembly/include/nimrtc/assembly/profiles.hpp).
The invariant is enforced by
[`AssemblyTest.JsonFileMatchesBuiltinConstant`](../src/modules/assembly/tests/test_assembly.cpp)
— any drift (e.g. an edit to `profiles/sfu.json` without updating
`kProfileSfu`) fails the build.

## Adding a new profile

1. Add `kProfile<Foo>` constant to
   [`profiles.hpp`](../src/modules/assembly/include/nimrtc/assembly/profiles.hpp).
2. Register it in
   [`assembly.cpp`](../src/modules/assembly/src/assembly.cpp) constructor
   (add to the `profiles_.emplace(...)` list).
3. Add the matching JSON file under `profiles/<foo>.json` with a header
   comment block explaining the use case and design rationale.
4. Add an invariant test (`Profile<Foo>DistinguishingFeature`) to
   `test_assembly.cpp` pinning the distinctive fields.
5. Update the matrix table at the top of this file.
6. If the profile introduces a new schema marker (e.g. `foo_enabled`),
   amend ADR-010 §"Schema v1.x incremental fields" with the additive
   contract before landing.
