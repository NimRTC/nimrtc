# Video Pipeline v2 Plan — Adaptive Quality, Codec Breadth, Hardware-First

> **This document is the single plan for the video-pipeline rewrite.** It covers
> everything that must land before the video path can be called production
> usable: the feedback→BWE→pacing→rate-control closed loop, the encoder /
> capture plug-in seams, H.265 and AV1 support, and hardware-first codec
> selection.
>
> Scope of this document: **work-item inventory** (Part A — every item that
> has to be done, numbered so nothing is lost) plus **version / milestone
> plan** (Part B). Part A is deliberately exhaustive; Part B says in which
> release each item ships.

| | |
|---|---|
| Plan opened | 2026-11-06 |
| Decisions closed | 2026-11-06 — the three owner decisions (OD-1 no bundled codecs, OD-2 hardware-first default, OD-3 profile→mode table + start sequencing) are recorded in **§B.7**. No item is blocked on a pending decision. |
| Baseline | `main` @ post-v1.1 consolidation (`docs/plan/v1.1-plan.md` is the v1.1 plan) |
| Scope class | 视频管线重构 — new public plugin seams, new wire-format support, no transport rewrite |
| First release | **v1.2.0** — development starts **after the v1.1.0 tag is cut** (OD-3(b)) |
| Last release in this plan | **v1.4.0** (v1.5.0+ carries the explicitly deferred items) |
| Owner | BDFL |
| Related | `docs/zh/architecture.md` §6, ADR-001 (plugin system), ADR-015 (API stability), ADR-017 (cross-end timeline) |

---

## 0. Decisions this plan is built on

These four answers came from the design discussion (2026-11-06) and are
binding for every work item below. Three further decisions (OD-1 … OD-3)
were taken the same day and are recorded in **§B.7**; they refine R2/R3 and
fix the start sequencing.

| # | Decision | Consequence for the work items |
|---|---|---|
| **D1** | **Bandwidth signal: support both, compatible.** Use `transport-cc` when the peer supports it; fall back to loss/RTT (REMB / RR) otherwise. | F3–F5 must implement a *signal-selection policy*, not two separate BWE stacks. One `IBwe` implementation, two input paths, one arbitration rule. |
| **D2** | **Degradation ladder: two modes.** *Quality-first* and *Smoothness-first*. | B3 ships two ladder policies behind one `IVideoBitrateController` interface; the mode is a configuration value, not a compile-time choice. |
| **D3** | **Encoder ownership: engine computes, encoder obeys.** The engine computes the full encoder parameter set (resolution / fps / bitrate / keyframe interval / QP) and pushes it; the encoder is passive and single-purpose. | B5 defines one `EncoderParams` struct computed in one place. The encoder plugin must **not** subscribe to bandwidth itself. |
| **D4** | **Target-scene priority: two modes selected by configuration.** | B4 maps scene profiles (`teleop`, `call`, …) onto the D2 modes through configuration, not through hard-coded per-profile branches inside the controller. |

Additional requirements stated alongside D1–D4:

| # | Requirement | Consequence |
|---|---|---|
| **R1** | The **encoder module** and the **capture module** must be pluggable, swappable at runtime. | C1/C2/C3 — new plugin seams in `src/plugins/`, implementations in `src/modules/<name>/`. `src/engine/` may only orchestrate (L4). |
| **R2** | **Prefer hardware codecs** wherever available. | C12–C16 — a hardware probe + capability query + hardware-preferred selection with software fallback. **Refined by OD-2: hardware is the default, software is the fallback, and a `codec_backend` override exists.** |
| **R3** | **H.265 and AV1 must both be supported.** | C4–C8 — payload format, SDP `fmtp`, codec seams, and negotiation for both. **Refined by OD-1: NimRTC ships the seams, payload, SDP and negotiation; the codec binary comes from the host or an out-of-tree plug-in.** |

### Constraint compliance (per AGENTS.md)

| Constraint | How this plan respects it |
|---|---|
| **L1** `src/core/` is the only INTERFACE library, header-only | No new core types are needed beyond a couple of value structs; those live in the owning module and are re-exported through `src/plugins/`. |
| **L2** `src/plugins/` holds interfaces and factory interfaces only | Every new seam (`IVideoBitrateController`, `IVideoEncoder`, `IVideoCapture`) is declared here with **no implementation**. |
| **L3** each module self-contained under `src/modules/<name>/` with its own CMakeLists + include/ + src/ + tests/ | All new modules follow the existing layout (`net_feedback`, `pacing`, `video_bitrate_control`, `video_capture`, `h265`, `av1`). |
| **L4** `src/engine/` orchestrates only, never includes concrete module headers | The engine reaches new components through `core::PluginRegistry` + the PAL resolver (`engine/pal/engine_plugin_resolver.hpp`), the same path as `bwe` / `audio3a` today. |
| **L5** `src/third_party/` is the only vendor location; `NIMRTC_VENDORED` stays ON | No new vendored video codec dependency is introduced: **OD-1 prohibits bundling a software H.265/AV1 encoder or decoder**, so this plan adds nothing to `src/third_party/`. The only vendor work is the existing out-of-tree hardware plug-ins (C15), which stay outside the core build. |
| **L6** no global mutable state outside `nimrtc::core::` | The rate controller, pacing bucket and feedback collector are all per-session instances owned by the engine. |
| **L7** protected domains (`policy/`, `.zelto/`, `AGENTS.md`) are not overridable by policy rules | No work item touches those paths. |

---

# Part A — Complete work-item inventory

Every item below has an ID, a landing site, and an acceptance criterion.
Part B maps the IDs onto releases. **An item is not done until its
acceptance criterion passes on all four CI legs** (Windows MSVC, Linux
x86_64, Linux aarch64, macOS — aarch64 per `docs/zh/architecture.md`
§13.1 "compiles ≠ interoperates").

Legend for landing sites: `P` = `src/plugins/`, `M` = `src/modules/<name>/`,
`E` = `src/engine/`, `T` = `tests/`, `X` = `tools/`, `D` = `docs/`.

---

## Track F — Network feedback and the BWE loop

> **Why first:** today `IBwe::on_feedback()` is called *only from unit tests*.
> The engine reads `bwe_->estimate()` in its tick, but nothing ever feeds
> the estimator, so `target_bitrate_bps` is pinned at
> `initial_bitrate_bps` (1 Mbps default) forever. Everything in Track B is
> meaningless until this track lands. The signal *policy* itself is D1.

| ID | Item | Landing | Acceptance |
|---|---|---|---|
| **F1** | RTCP demux on the receive path: parse inbound RTCP compound packets in the engine, route SR / RR / PSFB / RTPFB by PT to the right consumer. | `E` | A loopback peer that sends an RR with a non-zero fraction-lost makes the engine's feedback counter move; unit test asserts the routing table. |
| **F2** | New module `net_feedback`: aggregate receiver-side network signals into one snapshot (fractional loss, cumulative lost, RTT, jitter, REMB bitrate, transport-cc arrival statistics). Owns no transport; consumes parsed RTCP. (Named `net_feedback` to avoid confusion with the existing `video_rtcp_feedback` module, which handles the *video-side* PSFB/RTPFB feedback, not sender-side network feedback.) | `M` `src/modules/net_feedback/` | Module unit tests: feed a synthetic RR + REMB + transport-cc sequence, assert the aggregated snapshot. |
| **F3** | Extend `plugins::BweFeedback` with the fields transport-cc needs (per-packet arrival delta, packet size, ack sequence span) and add an explicit `signal_source` discriminator (`kLossRtt`, `kTransportCc`, `kRemb`). Additive only — `NIMRTC_API_STABLE` symbols are not removed. | `P` | `tests/test_api_version.cpp` + `api_freeze_scan` pass; existing BWE implementations still compile unchanged. |
| **F4** | Use the existing `rtp::Parser::parse_transport_cc` in production (today it has no caller) to build the arrival-time series F2 needs. | `M`/`E` | `parse_transport_cc` is referenced from non-test code; a synthetic transport-cc packet yields a non-empty arrival series. |
| **F5** | **Signal-selection policy (D1)**: negotiate whether the peer supports `transport-cc` (via `a=rtcp-fb` in SDP) and select the driving signal — transport-cc when available, loss/RTT + REMB otherwise. Policy is a plug-in-configurable choice with an automatic default. | `P` + `M` | Table-driven test: peer supports transport-cc → policy reports `kTransportCc`; peer does not → `kLossRtt`; both paths feed the same `IBwe`. |
| **F6** | Wire the loop: ICE/DTLS transport → RTCP demux → `net_feedback` → `bwe_->on_feedback()` → `scheduler_->on_bwe_update()`. | `E` | End-to-end loopback test: a lossy channel produces a `target_bitrate_bps` that **changes** over time (currently it stays at the initial value — that is the bug this track closes). |
| **F7** | REMB parsing + trust policy: the existing REMB-override logic in `GoogCcBwe` (`kRembTrustFactor = 0.8`) is kept, but REMB becomes one *input* rather than the only receiver-side opinion. | `M` | Regression test keeps the current REMB-override semantics. |
| **F8** | BWE re-targeting for a *single* video stream vs. the whole session: the current `IBwe` returns one estimate for the session. Define how per-stream shares are derived (see B2). | `E` (engine-private, header-only) | The rule is documented (`src/engine/src/bandwidth_allocation.hpp`) and unit tested — 16 cases in `tests/test_bandwidth_allocation.cpp`. The engine's audio path uses it, so there is one rule and not two. |

## Track P — Pacing, queueing and back-pressure

> **Why:** `sched::on_bwe_update()` computes `budget_bytes = target_bps / 8`
> — *per second* — and `drain_with()` releases at most 64 packets per tick.
> There is no time-based rate shaping, so bursts leave at line rate and only
> converge by losing packets. On a weak network this destroys the video
> stream even when BWE has correctly lowered the target.

| ID | Item | Landing | Acceptance |
|---|---|---|---|
| **P1** | New module `pacing`: token-bucket pacer on a monotonic clock. `N` tokens accumulate at `pacing_bps`; burst is bounded. | `M` `src/modules/pacing/` | Unit test: over 1 s of simulated time the pacer releases at most `pacing_bps/8` bytes ± one bucket of slack, and never exceeds the burst ceiling. |
| **P2** | Integrate the pacer with `sched`: video / audio / data-channel buckets are paced independently; RTX and RTCP feedback stay in the high-priority bucket and bypass the pacer. | `M`/`E` | Scheduler test asserts the high-priority bucket is never delayed by a saturated video bucket. |
| **P3** | **Encoder back-pressure**: the encoder → send-queue → pacer chain each hold a rate view; when the send queue exceeds its budget the *encoder* is asked to skip frames, rather than letting the network drop them. | `E` + `P` | Under a constrained target the encoder's `frames_skipped` counter rises; the network loss counter stays flat. |
| **P4** | Pacing telemetry: bytes-per-second actually emitted, queue depth, drops-by-back-pressure — surfaced through `video_stats` (see O1). | `M`/`E` | Stats snapshot exposes the new fields. |

## Track B — Rate control (the ABR core)

> **Scope class of this track:** D2 (two ladders), D3 (engine computes,
> encoder obeys), D4 (mode chosen by configuration).

| ID | Item | Landing | Acceptance |
|---|---|---|---|
| **B1** | New plugin seam `IVideoBitrateController` + `IVideoBitrateControllerFactory` in `src/plugins/`. Interface only (L2). | `P` | `src/plugins/` contains no implementation code; the plugin-author guide gains a section. |
| **B2** | New module `video_bitrate_control` implementing the controller: holds the resolution ladder, the frame-rate ladder, the bitrate ladder, and the current level. Consumes the BWE estimate (per F8's share rule) + the measured outbound bitrate (from O1) + keyframe-request feedback. | `M` `src/modules/video_bitrate_control/` | Module unit tests: given a synthetic bandwidth drop, the controller steps down the ladder; given recovery, it steps up with hysteresis. |
| **B3** | **Two ladder modes (D2)**. *Quality-first*: keep resolution and frame rate, reduce bitrate / QP first, drop frame rate only at the floor. *Smoothness-first*: keep frame rate and motion cadence, reduce resolution first, bitrate second. Both are pure functions of (bandwidth estimate, current level, scene config). | `M` | Same input trace, two modes → two different level sequences; both are captured as golden traces in the module test. |
| **B4** | **Configuration-driven mode selection (D4 / OD-3(a))**: the ladder mode is a configuration value (`quality` / `smoothness`), settable per engine, and defaulted from the scene profile (`profiles/*.json`) rather than hard-coded inside the controller. The concrete default per profile is the table in **§B.7.3** (`teleop` / `agent` / `agent-low-latency` → smoothness-first; `call` / `live` / `agent-gateway` / `relay-agent` → quality-first; engine default → quality-first). | `P` + `M` + `profiles/` | Two profiles using different modes produce measurably different ladder behaviour under the same bandwidth trace; the profile file carries the mode; every shipped profile resolves to the mode the §B.7.3 table prescribes. |
| **B5** | **Unified encoder parameter computation (D3)**: one place computes the complete parameter set for the encoder — `width`, `height`, `fps`, `bitrate_bps`, `keyframe_interval`, `qp`/quality, and the codec-specific extras. Output is one value struct; the encoder receives it and nothing else. | `P` (struct) + `M` (computation) | The encoder plug-in interface exposes a single `set_params()`-style entry point; it has no access to `IBwe`, to the controller, or to the feedback stream. |
| **B6** | **Timestamp continuity under frame-rate reduction**: the RTP timestamp is driven by a single capture clock at 90 kHz, so dropping frames changes the *frame count*, never the timestamp step. | `E` | Test: reducing 30 → 15 fps produces RTP timestamps that advance by exactly 6000 ticks per delivered frame, and no gaps in the 90 kHz timeline. |
| **B7** | **Dynamic keyframe interval**: the controller raises the keyframe interval when bandwidth is healthy and lowers it when a keyframe is repeatedly requested; requests are rate-limited (the existing `FirCoalescer` is finally wired into the engine — the PLI storm feedback loop is closed). | `M`/`E` | A peer that requests keyframes every 100 ms for 1 s produces at most the rate-limited number of IDRs; no runaway IDR rate. |
| **B8** | **Parameter-set refresh on resolution change**: a resolution change requires new codec parameter sets (H.264 SPS/PPS, H.265 VPS/SPS/PPS, AV1 sequence header). The sender must emit them with the new frame. | `M` | After a ladder step that changes resolution, the receiver can decode without a PLI round-trip; asserted in a pipeline test. |
| **B9** | **Controller safety rails**: minimum floor bitrate, maximum step per interval, hysteresis band, and a cooldown so the ladder cannot oscillate. | `M` | A sawtooth bandwidth trace does not produce a staircase that flips level every interval; a test asserts the level-change rate is bounded. |
| **B10** | **Interaction with `sched` / pacing**: the controller's bitrate target and the pacer's pacing rate are the same number (pacing slightly above target is allowed and documented). | `E` | No drift between `bwe_->estimate().target_bitrate_bps` and the pacer's configured rate; asserted by an engine test. |

## Track C — Codec plug-ins, codec breadth, hardware-first

> **Scope class of this track:** R1 (encoder + capture are plug-ins),
> R2 (hardware-first), R3 (H.265 + AV1).

### C.1 Plug-in seams

| ID | Item | Landing | Acceptance |
|---|---|---|---|
| **C1** | **Capture plug-in seam**: a real device-capture interface (`IVideoCapture`: device enumeration, format negotiation, start/stop, frame delivery). The current `IVideoSource` (memory test-pattern generator) stays as the reference implementation. | `P` + `M` | A third-party capture plug-in can be registered and driven by the engine without touching `src/engine/` or `src/modules/`. |
| **C2** | **Encoder plug-in seam**: split the current combined `IVideoCodec` (encode + decode in one object) into an **encoder** interface with the passive contract of B5, and a **decoder** interface. The existing `IVideoCodec` remains as a compatibility shim for one minor cycle (ADR-015 deprecation rule). | `P` | Old code using `IVideoCodec` still compiles; the new encoder interface has no bandwidth-awareness. |
| **C3** | **Decoder plug-in seam** (split from C2) with capability query (supported profiles, resolutions, formats) so the engine can reject a configuration up front. | `P` | Engine rejects an unsupported resolution with a clear error instead of failing at first frame. |

### C.2 Codec breadth

| ID | Item | Landing | Acceptance |
|---|---|---|---|
| **C4** | **H.265 payload format** (RFC 7798): packetizer + depacketizer (single NAL, AP, FU), mirroring the existing H.264 module layout. | `M` `src/modules/h265/` | Round-trip test: a fragmented NAL unit reassembles byte-identically; FU reassembly in the video jitter buffer mirrors the H.264 FU-A path. |
| **C5** | **H.265 SDP attributes**: `profile-id`, `tier-flag`, `level-id`, `interoperability constraints` — parse + build, added to `video_sdp_attributes`. | `M` | Parse/build round-trip tests; the module's header comment stops saying "H.264/VP8/VP9 only". |
| **C6** | **AV1 payload format** (draft-ietf-payload-av1): OBU aggregation, payload descriptor, and the AV1 aggregation-header rules. | `M` `src/modules/av1/` | Round-trip test on a synthetic AV1 frame with a descriptor. |
| **C7** | **AV1 SDP attributes** (`level-idx`, `tier`, `profile`, `bit-depth`, `monochrome`, …) parse + build. | `M` | Parse/build round-trip tests. |
| **C8** | **H.265 / AV1 codec seams**: `nimrtc_h265` / `nimrtc_av1` modules implementing the C2/C3 seams. **No bundled software encoder or decoder ships** (OD-1): the seam, the payload layer, the SDP layer and the negotiation must be complete and tested, and the codec implementation is supplied by the host application or an out-of-tree plug-in. A minimal **test-only** codec that emits syntactically valid (non-compressing) bitstreams is provided for the test suite and the interop harness; it is not part of the installed library. | `M` + `P` | A reference plug-in registered under a test id round-trips a frame through the full pipeline; `cmake --install` output contains no H.265/AV1 encoder binary. |
| **C9** | **SDP negotiation is two-way** (fixes the current hard-coding at `engine_signaling.cpp:153`, which writes `H264/90000` unconditionally): the offer carries the negotiated codec list + `fmtp`, and the answer is validated against it. `video_sdp_attributes` is finally wired into the engine. | `E` + `M` | Offer with `H265` + `AV1` + `VP8` payload types produces a correct answer; the engine selects a codec both sides support. |
| **C10** | **VP8 / VP9 pipeline wiring**: the packetizers exist but the pipeline treats VP8/VP9 as opaque partitions and has no codec. Wire the existing VP8/VP9 depacketizers into the pipeline properly (VP9 picture-id-based frame boundary detection instead of the current approximation). | `M` | A VP9 frame with a picture id is delivered as one complete frame by the jitter buffer. |
| **C11** | **Payload-type / SSRC demultiplexing by negotiated result**: today the engine distinguishes audio from video by a locally configured payload type, not by the negotiated answer; a mismatch silently kills video receive. | `E` | A mis-configured local PT no longer silently breaks video; the negotiated mapping is used, and a mismatch is reported. |

### C.3 Hardware-first

| ID | Item | Landing | Acceptance |
|---|---|---|---|
| **C12** | **Hardware probe**: enumerate available hardware encode/decode backends (NVENC, NVDEC, AMF, QSV, VA-API, DXVA, MediaCodec, VideoToolbox, OpenH264-on-CPU) and report which are usable in the current process. **Required, not optional** — it is the mechanism that makes the hardware-first default of OD-2 work. | `M` | `hardware_backend()` on each registered plug-in returns a truthful value; a probe API lists the available backends. |
| **C13** | **Capability query**: per backend, the supported codec / profile / resolution / fps / pixel-format matrix. | `P` + `M` | The engine picks a backend that supports the requested configuration, or reports exactly which constraint failed. |
| **C14** | **Hardware-preferred selection with software fallback (R2 / OD-2)**: hardware is the **default** — the engine picks a hardware encoder whenever one exists and supports the configuration; software is the fallback, not the default. A configuration override (`codec_backend: auto | hardware | software`; `auto` = hardware-first) forces a specific path. The selected backend is recorded in the telemetry (O1). | `E` | On a machine with no hardware encoder the pipeline still runs (software path) and says so; on a machine with one, the hardware path is selected and reported; an explicit `software` override really does use software. |
| **C15** | **Hardware coverage for the new codecs**: extend the out-of-tree hardware plug-ins (currently H.264-only) to H.265 (NVENC HEVC, VA-API, MediaCodec, VideoToolbox HEVC) and, where a hardware path exists, AV1. | out-of-tree plug-ins | Each supported backend encodes one H.265 frame end-to-end; unsupported combinations are explicitly reported rather than silently ignored. |
| **C16** | **Zero-copy GPU path completed**: `VideoFrame`'s `GpuBuffer` reaches the hardware encoder surface without a CPU round-trip when the backend supports it (`zero_copy_supported`). | `P` + `M` | The existing zero-copy surface test covers the H.264 path; the H.265 path follows the same contract. |

## Track R — Receive-side quality

| ID | Item | Landing | Acceptance |
|---|---|---|---|
| **R1** | **`video_stats` wired into the engine** (the module exists and says so itself in its own header). Provides measured outbound bitrate, frame counts, dropped frames, keyframe requests, inbound freeze detection. This is the feedback the controller needs (B2) and the ground truth for O2. | `E` | The engine exposes a per-stream stats snapshot; values change during a live session. |
| **R2** | **Decoder-side resolution switch**: the receiver must handle a parameter-set change (H.264/H.265 SPS·PPS·VPS, AV1 sequence header) mid-stream without a full restart. | `M` | A synthetic mid-stream resolution change is decoded without an error. |
| **R3** | **Render clock + A/V sync**: turn the existing skew observation (`SenderReportTracker`, ADR-017) into action — a render clock, and align / drop / reorder decisions driven by the measured skew. | `M` (new `video_sync`) + `E` | A/V skew that exceeds the configured tolerance produces a documented drop or delay correction, asserted in a test with a synthetic skew. |
| **R4** | **Receiver-side decode degradation**: when the receiver cannot keep up, drop frames rather than let the buffer grow unboundedly (bounded-memory guarantee). | `M` | Memory stays bounded under a sustained decode overrun; the drop count is reported. |

## Track T — Transport robustness

| ID | Item | Landing | Acceptance |
|---|---|---|---|
| **T1** | **RTX** (RFC 4588) as a real retransmission payload type, negotiated via `a=rtpmap` + `a=fmtp:apt=`. Today NACK is answered by re-sending the original packet, which is not RTX. | `M` (new `rtx`) + `E` | A NACK for a lost packet is answered by an RTX packet on the RTX payload type; the receiver restores the original sequence number. |
| **T2** | **PLI / FIR / SLI feedback loop hardened** (see B7): the receive side stops asking for keyframes it cannot get. | `M`/`E` | No IDR storm under a sustained loss profile; asserted over a 30 s synthetic-loss run. |
| **T3** | **ULPFEC / FlexFEC / RED** | — | **Deferred to v1.5.0+** (§O-2). No work item in this plan. |
| **T4** | **Simulcast / SVC** | — | **Deferred to v1.5.0+** (§O-2). |

## Track O — Observability, benchmarks, interoperability

| ID | Item | Landing | Acceptance |
|---|---|---|---|
| **O1** | **Per-stream observability surface**: actual bitrate, target bitrate, current ladder level, pacing rate, queue depth, keyframe rate, encode/decode errors, freeze count, A/V skew — all queryable from the engine. | `E` + `M` | A single stats call returns a coherent snapshot; used by the benchmarks in O2. |
| **O2** | **Weak-network benchmark for the adaptive path**: extend `tools/benchmark_weaknet.py` with a video scenario that applies loss / jitter / rate limits and reports the *response curve* (target bitrate vs. time, ladder level vs. time, freeze count, end-to-end latency), not just a throughput number. | `X` | The tool emits a JSON artifact; the README benchmark table cites it. |
| **O3** | **Chromium interop for the new codecs**: Chrome supports H.264 and VP8/VP9/AV1 negotiation (AV1 support depends on the build); H.265 interop is limited. Verify what the negotiation actually produces against a real Chrome peer. | `X` + `tests/chrome_interop_harness/` | The interop harness records the negotiated codec for at least H.264 and one non-H.264 codec; H.265 is documented as "negotiated but not decodable by the reference peer" if that is what the harness observes. |
| **O4** | **Test matrix**: unit tests per module, integration tests across the pipeline, engine-level loopback tests, and the four CI legs. | `T` | Every item's acceptance criterion is backed by a named automated test. |
| **O5** | **Quality report generator**: a script that runs the weak-network scenario and emits a before/after comparison against the v1.1 baseline, so the adaptive loop's benefit is measurable rather than asserted. | `X` | A committed artifact shows the closed loop improving (lower freeze count, lower latency) versus the fixed-bitrate baseline. |

## Track G — Architecture governance, documentation, API

| ID | Item | Landing | Acceptance |
|---|---|---|---|
| **G1** | **ADR-018** — bandwidth signal policy (D1): transport-cc preferred, loss/RTT fallback, arbitration rules. | `docs/adr/` | ADR accepted; the policy is referenced from the module headers. |
| **G2** | **ADR-019** — encoder ownership (D3) + ladder modes (D2) + configuration mapping (D4 / OD-3(a), including the profile→mode table of §B.7.3). | `docs/adr/` | ADR accepted; the profile→mode table is reproduced in the ADR so the mapping has a canonical home. |
| **G3** | **ADR-020** — codec plug-in architecture (R1), hardware-first selection (R2 / OD-2, including the `codec_backend` override and the hardware-preferred default), and the **no-bundled-codec posture (OD-1)** — plus the deprecation path for `IVideoCodec`. | `docs/adr/` | ADR accepted; the deprecation timeline follows ADR-015 (one minor cycle of `NIMRTC_API_DEPRECATED` before removal); the ADR states that NimRTC bundles no software H.265/AV1 encoder or decoder. |
| **G4** | **`docs/zh/architecture.md` §6 rewrite**: the media-quality section must describe the closed loop, the two modes, and the codec matrix as they actually exist after this plan. | `D` | §6 no longer describes the video path as "P1 minimum usable only". |
| **G5** | **README / README.zh.md feature tables** updated with the codec matrix and the adaptive-quality claim, with the experimental-labelling rule of §11.1 respected. The H.265 / AV1 rows must say what OD-1 makes true: "negotiated and transported — bring your own codec", not "supported". | `D` | Feature tables match the shipped state; no row implies a bundled codec that does not exist. |
| **G6** | **Plug-in author guide** (`docs/plugin_author_guide.md`) gains sections for the new seams (capture, encoder, bitrate controller) and the hardware-selection contract — including how to supply an H.265 / AV1 codec under OD-1. | `D` | A reader can write a capture plug-in, a hardware encoder plug-in, and an H.265 codec plug-in from the guide alone. |
| **G7** | **API stability bookkeeping**: every new public symbol carries `NIMRTC_API_STABLE` / `NIMRTC_API_UNSTABLE`; `tests/test_api_classification.cpp` baseline updated; the public-header count test updated. | `T` + `P` | `api_freeze_scan` and the header-count test pass with the new headers included. |
| **G8** | **CMake option hygiene**: each new module gets an `NIMRTC_MODULE_<NAME>` option (default ON) consistent with the existing ones; new vendor dependencies get a `NIMRTC_VENDORED_<NAME>` option (L5). | `CMakeLists.txt` | `cmake -LA` lists the new options; a build with each module OFF still configures. |

## Track X — Explicitly deferred (recorded, not in scope here)

| ID | Item | Why deferred | Target |
|---|---|---|---|
| **O-1** | Screen-share capture, multi-device capture, camera hot-plug | Capture seam (C1) lands first; device breadth is a separate effort. | v1.4.0+ |
| **O-2** | ULPFEC / FlexFEC / RED, simulcast / SVC | Need the codec matrix and the pacing work to be right first; they multiply the number of interacting variables. | v1.5.0+ |
| **O-3** | **Bundling a software H.265 / AV1 encoder or decoder** | **CLOSED by OD-1 — this is now a prohibition, not a deferral.** NimRTC ships no bundled software video encoder/decoder for H.265 or AV1; x265 (GPL-2.0) is incompatible with the permissive distribution model, and permissive AV1 encoders add unacceptable binary weight. Host application or out-of-tree plug-in supplies the codec. Revisit only via an ADR amendment. | closed |
| **O-4** | A second bundled software H.264 encoder beyond the existing OpenH264 option | The existing `NIMRTC_PLUGINS_OPENH264` option already covers the software fallback need. | — |
| **O-5** | Server-side SFU video forwarding, video in the relay path | SFU track stays closed per `docs/plan/v1.0-plan.md` §7. | v1.5.0+ |
| **O-6** | End-to-end hardware encode on every supported CI leg | The CI legs do not all have a hardware encoder; hardware coverage is validated on a dedicated job or manually, and the software path is what CI enforces. | v1.3.0 |

---

# Part B — Version and development plan

## B.1 Release map

| Release | Theme | Tracks | Headline claim |
|---|---|---|---|
| **v1.2.0** | **Adaptive quality closed loop** | F (all), P (P1–P3), B (all), R1, O1, O2, O5, G1, G2, G7, G8 | "Bandwidth-adaptive video: the target bitrate tracks the network, the ladder degrades predictably, and the encoder is told what to do." |
| **v1.3.0** | **Codec breadth and hardware-first** | C (all), R2, O3, G3, G4, G5, G6 | "H.264 / H.265 / AV1 / VP8 / VP9 negotiate properly, and hardware codecs are used automatically when the machine has them." |
| **v1.4.0** | **Device layer and receive experience** | C1 (device breadth), R3, R4, T1, T2, O4 | "A real camera in, a real renderer out, with A/V synchronisation and RTX-based recovery." |
| **v1.5.0+** | Deferred items | O-2, O-5, O-1 | FEC, simulcast/SVC, SFU video. |

Each release is independently shippable: **v1.2.0 is useful on its own**
(the closed loop is the biggest single quality win), and v1.3.0 / v1.4.0
add breadth without depending on unpublished work.

## B.2 v1.2.0 — internal milestone order

The order is dependency-driven, not preference-driven. Each milestone ends
with a green runnable state; a milestone may not start before the previous
one's acceptance criteria pass.

| Milestone | Items | Ends when | Dependency note |
|---|---|---|---|
| **M1 — Feedback ground** | F1, F2, F3, F4, F7 | The engine parses inbound RTCP and the BWE estimate **moves** under a lossy channel. | Nothing else in the plan can work before this. |
| **M2 — Signal policy** | F5, F6, F8 | transport-cc is used when the peer offers it; loss/RTT is used otherwise; the same `IBwe` serves both. | Needs the SDP `a=rtcp-fb` negotiation in place (part of C9, but the *negotiation-only* slice lands here). |
| **M3 — Pacing** | P1, P2 | Emitted bytes are shaped by a token bucket; the high-priority bucket is unaffected. | Needs M1's target rate. |
| **M4 — Rate controller** | B1, B2, B3, B4, B5, B9 | The controller produces a ladder level and a parameter set from a synthetic bandwidth trace, in both modes. | Needs M2's estimate and M3's rate view. |
| **M5 — Encoder hand-off** | B5 (integration), B6, B7, B8, C2 (encoder seam), P3, P4 | The engine computes the parameters and the encoder obeys them; frame dropping preserves the 90 kHz timeline; keyframe requests are rate-limited. | Needs M4 and the encoder seam. |
| **M6 — Observability** | R1, O1, O2 | Stats are exposed by the engine and the weak-network benchmark produces the response-curve artifact. | Needs M5 to have something to measure. |
| **M7 — Proof** | O5, O4 | The before/after quality report shows the closed loop beating the fixed-bitrate baseline. | Needs M6. |

**v1.2.0 definition of done:** the four CI legs are green; the weak-network
benchmark artifact is committed; `docs/zh/architecture.md` §6 is rewritten;
the ADR-018 / ADR-019 are accepted; the v1.2.0 release notes list the new
public API with the ADR-015 stability markers.

## B.3 v1.3.0 — internal milestone order

| Milestone | Items | Ends when |
|---|---|---|
| **N1 — H.265** | C4, C5, C8 (H.265 half) | An H.265 frame survives packetize → jitter buffer → depacketize → decode, and the SDP carries the right `fmtp`. |
| **N2 — AV1** | C6, C7, C8 (AV1 half) | Same for AV1, including the aggregation header. |
| **N3 — Negotiation** | C9, C11, C10 | The offer/answer round trip selects a mutually supported codec from a multi-codec list; VP9 frames are delivered whole. |
| **N4 — Hardware-first** | C12, C13, C14, C15, C16 | The engine prefers a hardware encoder that supports the configuration, falls back to software otherwise, and says which one it used. |
| **N5 — Plug-in seams** | C1, C3 | A third-party capture plug-in and a third-party decoder plug-in can be registered and driven by the engine. |

## B.4 v1.4.0 — internal milestone order

| Milestone | Items | Ends when |
|---|---|---|
| **Q1 — Receive-side** | R2, R4 | A mid-stream parameter-set change decodes cleanly; the receive buffer is bounded under overrun. |
| **Q2 — A/V sync** | R3 | Measured skew drives an explicit align/drop decision, asserted in a test. |
| **Q3 — RTX** | T1, T2 | A lost packet is recovered through a real RTX payload type; no IDR storm under sustained loss. |
| **Q4 — Device breadth** | O-1 subset | A camera capture path exists behind the C1 seam. |

## B.5 Cross-cutting work that is not milestone-bound

These run continuously across all three releases and are owned by whoever
touches the affected area:

- **API bookkeeping (G7)** — every new public symbol needs a stability
  marker the day it is written, not at release time.
- **Test-first** — each item's acceptance criterion names a test; the test
  is written with the item, not after it.
- **aarch64 honesty** — `docs/zh/architecture.md` §13.1: a green aarch64
  compile is not interop. Codec work that claims aarch64 support must have
  an interop run, or the claim is worded as "compiles".
- **Documentation drift** — the module header comments are the de-facto
  documentation; they must not keep claiming "not yet wired into the
  engine" after the engine wires them.

## B.6 Risks and the pre-agreed response

| Risk | Impact | Response |
|---|---|---|
| **R-1** The closed loop is built but the ladder oscillates, making the picture worse than a fixed bitrate. | High (visible regression) | B9's hysteresis + cooldown are acceptance criteria, not nice-to-haves; the golden-trace tests in B2/B3 lock the behaviour down. |
| **R-2** Encoder seams change breaks existing users of `IVideoCodec`. | Medium | `IVideoCodec` stays as a deprecated shim for one minor cycle (ADR-015); the new interfaces are additive. |
| **R-3** The H.265 / AV1 codec work stalls on licensing. | Medium | **Closed by OD-1**: the question is settled — no bundled binaries, host supplies the codec. The payload / SDP / negotiation / seam layers are unaffected by that choice, and the deliverable no longer depends on a codec binary existing. Residual risk is only that a deployment has no H.265 codec to plug in, which is the host's call, not a NimRTC blocker. |
| **R-4** Hardware coverage differs per CI leg, so "hardware-first" cannot be CI-verified everywhere. | Medium | The software path is what CI enforces; hardware paths are validated on a dedicated job and reported honestly (O-6). |
| **R-5** Scope growth: the video plan swallows the whole roadmap. | High | The version map is fixed here; items outside the three releases are recorded in Track X as deferred, not silently dropped. |
| **R-6** v1.1.0 is still in flight and this plan's foundation work touches the same files (`engine_transport.cpp`, `engine_media.cpp`, `engine_signaling.cpp`). | Medium | **Closed by OD-3(b): v1.2.0 development starts only after the v1.1.0 tag is cut.** v1.1.0 is release-hardening; running the two plans concurrently on the same files is exactly what this risk describes. The plan's first executable step is the v1.1.0 tag, not a code change. |

## B.7 Decisions taken by the owner (2026-11-06)

The three questions this plan originally left open are now decided. They are
binding on the work items below.

### B.7.1 OD-1 — No bundled software video encoders

**Decision.** NimRTC ships **no bundled software video encoder or decoder**
for H.265 or AV1. The seams, payload layers, SDP layers and the negotiation
are complete and tested; the *codec implementation* is supplied by the host
application or by an out-of-tree plug-in.

**Why.** x265 is GPL-2.0, which is incompatible with NimRTC's permissively
licensed distribution model (AGENTS.md 禁区: no system-installed codec
libraries bypassing the vendor policy; `docs/zh/architecture.md` §11.3 / §11.7).
Permissive AV1 encoders (libaom, SVT-AV1) exist but add significant binary
weight to a library whose size is a published selling point
(`tools/benchmark_size.py`).

**Consequences.**

- O-3 is **closed** as "do not bundle"; it leaves Track X and becomes a
  standing constraint. It is no longer a deferred item with a decision
  pending — it is a *prohibition*.
- C8 / C8's acceptance criterion is reworded: the deliverable is the
  **seams + a reference plug-in for tests**, never a shipped binary.
- The reference implementation used by the test suite and by the interop
  harness may be a minimal in-tree *test-only* codec that produces
  syntactically valid bitstreams (it does not compress). It lives under
  `tests/` or behind a `NIMRTC_MODULE_*_TESTCODEC` switch and is **not**
  part of the installed library.
- The docs (G4, G5, G6) must state plainly that H.265 / AV1 support means
  "NimRTC negotiates and transports them; bring your own codec". This is
  the same posture the project already takes for VP8 / VP9.

### B.7.2 OD-2 — Hardware-first is ON by default

**Decision.** The engine prefers a hardware codec whenever one exists and
supports the requested configuration. Software is the **fallback**, not the
default. An explicit per-engine / per-profile override is available to force
the software path.

**Why.** R2 states the requirement directly ("能用硬件编解码，尽量用硬件"),
and the fallback keeps the library usable on machines with no hardware
codec without the caller having to know in advance.

**Consequences.**

- C14 is reworded from "automatic selection with software fallback" to
  "hardware-preferred selection with software fallback, override available".
- The override is a configuration value alongside the ladder mode (B4),
  e.g. `codec_backend: auto | hardware | software`. `auto` = hardware-first.
- The selected backend (hardware or software, and which one) is reported in
  the telemetry (C14 / O1), because "why is this call blurry" is answered by
  "the hardware encoder was unavailable" only if that fact is observable.
- C12 (hardware probe) is therefore **not optional** — it is the mechanism
  that makes the default work. It lands in v1.3.0 with C14.
- On CI legs without a hardware codec the software path is what CI enforces
  (unchanged, see O-6 and risk R-4).

### B.7.3 OD-3 — Profile → ladder-mode table, and start after the v1.1.0 tag

**Decision (a) — the profile table.** The default ladder mode per scene
profile is:

| Profile | Ladder mode | Rationale |
|---|---|---|
| `teleop` | **smoothness-first** | Remote operation: a late-but-fresh frame is worth more than a sharp-but-stale one. Resolution is sacrificed first so the motion cadence and the control loop stay responsive. |
| `call` | **quality-first** | Human conversation: face detail carries the perceived quality; frame-rate reduction is more visible than mild resolution reduction. |
| `live` | **quality-first** | Same reasoning as `call`, with a higher floor on the bitrate (broadcast-style latency tolerance). |
| `agent`, `agent-low-latency` | **smoothness-first** | The consumer is a machine or a latency-critical loop; arrival rate beats fidelity. |
| `agent-gateway`, `relay-agent` | **quality-first** | The gateway re-encodes; preserving input fidelity matters more than the local preview. |
| `relay`, `transport` | n/a (no video ladder) | Media-forwarding profiles do not run a local encoder ladder; see §2.2 rule 3. |
| `call`, `live`, `teleop`, … (any profile not listed) | **quality-first** | Default when a profile carries no explicit mode. |

The table lives in `profiles/*.json` as **data**, so a deployment can add or
override a mapping without touching C++ code. The engine's own default,
when no profile is loaded, is **quality-first**.

**Decision (b) — start after the v1.1.0 tag.** v1.2.0 development begins
**after** the v1.1.0 tag is cut. v1.1.0 is release-hardening and the two
plans touch the same engine files (`engine_transport.cpp`, `engine_media.cpp`,
`engine_signaling.cpp`); running them concurrently is what risk R-6 warns
about. This closes risk R-6 as a *sequencing constraint* rather than a
live risk.

**Consequences.**

- B4's acceptance criterion gains the concrete table above; the profile
  JSON files gain a `video_ladder_mode` field.
- B3 (two ladder implementations) and B4 (configuration mapping) both
  become fully specified — no item in this plan is blocked on a pending
  decision any more.
- The plan's first executable step is **not** a code change: it is cutting
  the v1.1.0 tag and closing the v1.1 DoD checklist in
  `docs/plan/v1.1-plan.md`.

---

## B.8 Status

All three originally-open decisions are closed (OD-1, OD-2, OD-3). **No item
in this plan is blocked on a pending owner decision.** Any future change to
OD-1 / OD-2 / OD-3 requires an ADR amendment (ADR-020 covers OD-2 and the
OD-1 posture, ADR-019 covers OD-3's ladder modes and the profile table).

### Progress

| Step | State |
|---|---|
| 1. Cut the **v1.1.0** tag (OD-3(b) precondition) | **Done** — the tag exists; v1.2.0 work is unblocked. |
| 2. Draft **ADR-018** (bandwidth signal policy, D1) | **Done** — `docs/adr/ADR-018-bandwidth-signal-policy.md`, status *Proposed* (acceptance waits on M1's aggregation and the policy's table-driven test). |
| 3. Open **v1.2.0 M1** (F1, F2, F3, F4, F7) | **In progress.** The transport-cc **parsing** groundwork for F4 has landed: `rtp::Parser::parse_transport_cc` was corrected against the draft's real wire format (the chunk header contains the status symbols; the receive deltas are variable-width and follow all the chunks), the `PacketStatus` enum gained wire symbol 3, and the RTCP padding is honoured. Covered by 15 cases in `src/modules/rtp/tests/test_transport_cc.cpp`.
| 3a. **F1 + F2 + F3 + F4 (producer side) + F6** | **Landed.** The inbound RTCP compound datagram is now demultiplexed on the receive path (`rtp::demux_rtcp()`, RFC 3550 §6.1), the receiver-side network signals (Receiver Report, REMB, transport-cc) reach a new collector module (`src/modules/net_feedback/`) through the `plugins::INetFeedback` seam, and the engine hands the aggregated snapshot to `IBwe::on_feedback()` once per feedback report. The transport-cc parser now has a production caller. `plugins::BweFeedback` grew the `signal_source` discriminator and the transport-cc fields. Covered by 10 cases in `src/modules/net_feedback/tests/test_net_feedback.cpp`. |
| 3b. **F7** (the REMB trust policy) | **Open.** The collector's source precedence is fixed and documented (transport-cc → loss/RTT → REMB); the *configurable* policy (which signal the negotiated `a=rtcp-fb` set allows) still has to move into the engine. |

| 3e. **F8** (the per-stream share rule) | **Landed.** The rule is now one documented, unit-tested function: `src/engine/src/bandwidth_allocation.hpp` (engine-private, header-only — it is a pure function of (estimate, demand list), so it needs no state, no clock and no translation unit). The session's estimate is split between the streams: the **reserved** ones first (the audio — a clamped share with a *hard* floor), the **elastic** ones share what is left (the video), split by weight with the largest-remainder (Hamilton) method so the pool is handed out exactly and deterministically. A reserved stream that claims more than the estimate holds puts the session in **deficit**, which is reported rather than hidden. The engine's audio path now uses this rule instead of its own arithmetic (one rule, not two), so the video's elastic share arrives with B2 on the same terms — the rate controller is handed a per-stream *budget*, not the raw estimate. Covered by 16 cases in `tests/test_bandwidth_allocation.cpp`. |

**Next executable step**:

1. **Track P** (pacing) — the scheduler still has no time-based rate
   shaping, so a correct estimate does not yet reach the wire as a
   paced stream.  P1 (the token bucket) and P2 (the per-bucket
   integration) are the next two items; B2 (the rate controller) is
   downstream of P3, because the controller needs the pacer's rate view
   to keep the two from drifting apart (B10).
2. **F5** (the loop's *production* verification: a lossy channel must move
   `target_bitrate_bps` over time) — the loop is closed in code, but the
   end-to-end proof that the estimate actually moves is still missing.

Everything else in this plan is downstream of M1/M2/M3.

---

## Appendix — Item index

| Track | Items | Release |
|---|---|---|
| F — feedback / BWE | F1, F2, F3, F4, F5, F6, F7, F8 | v1.2.0 |
| P — pacing / back-pressure | P1, P2, P3, P4 | v1.2.0 |
| B — rate control | B1, B2, B3, B4, B5, B6, B7, B8, B9, B10 | v1.2.0 |
| R — receive side | R1, R2, R3, R4 | R1 → v1.2.0; R2/R4 → v1.4.0; R3 → v1.4.0 |
| C — codec / hardware | C1, C2, C3, C4, C5, C6, C7, C8, C9, C10, C11, C12, C13, C14, C15, C16 | v1.3.0 (C1 seam → v1.4.0 breadth) |
| T — transport robustness | T1, T2 | v1.4.0 |
| O — observability / interop | O1, O2, O5 | v1.2.0; O3 → v1.3.0; O4 → continuous |
| G — governance | G1, G2 | v1.2.0; G3 → v1.3.0; G4–G8 spread across both |
| X — deferred | O-1, O-2, O-4, O-5, O-6 | v1.4.0+ / v1.5.0+ / — |
| X — closed | O-3 | **closed by OD-1** — bundling is prohibited, not deferred |
