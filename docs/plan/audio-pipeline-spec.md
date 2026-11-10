# Audio Pipeline Specification — receive path, send path, and the P1 public SDK

> **What this document is.** The binding specification for the NimRTC audio
> pipeline: what the receive path must do (decode, conceal, keep the clock),
> what the send path must do (frame, timestamp, negotiate), which public API
> surface P1 adds, and how each item is verified.
>
> Scope of this document: **the specification and the design** (Parts A–G
> plus Part L, and the verification part V). **The development plan and the
> release/version plan are deliberately NOT in this document** — they are
> decided separately, once this spec is approved. Part G therefore contains
> dependency ordering only, not a schedule.
>
> Every work item is numbered so that nothing gets lost between this
> document and the eventual plan:
>
> | Prefix | Meaning | Section |
> |---|---|---|
> | `DEC-x` | Owner decision (binding) | §0 |
> | `F-x` | Confirmed defect / missing capability in the current tree | §1 |
> | `A-x` | P0 work item | Part A |
> | `B-x` | P1 work item | Part B |
> | `C-x` | P2 work item | Part C |
> | `E-x` | Public SDK item | Part E |
> | `L-x` | Logging / observability item | Part L |
> | `V-x` | Verification item | Part V |
> | `X-x` | Explicitly out of scope | §2.2 |
> | `O-x` | Open item for the owner (has a default) | §12 |

| | |
|---|---|
| Plan opened | 2026-11-06 |
| Baseline | `main` @ post-v1.1 consolidation (`docs/plan/v1.1-plan.md`) |
| Scope class | Audio pipeline — new plugin seams, wire-format completion, no transport rewrite |
| Status | **Specification — owner decisions confirmed 2026-11-06; the development plan and the version plan are written (`docs/plan/audio-pipeline-dev-plan.md`; the ladder amendment was fixed by the owner on 2026-11-06 — the audio P0+P1 body ships in `v1.2.0`). Amended 2026-11-10 (owner): Part L (logging and observability) added, C-4 (engine-internal A/V synchronisation) and C-5 (G.711) moved into the `v1.2.0` body** |
| Owner | BDFL |
| Related | ADR-001 (plugin system), ADR-008 (PCM tap), ADR-015 (API stability), ADR-017 (cross-end timeline), `docs/plan/video-pipeline-v2-plan.md` (same shape, video side) |

---

## 0. Decisions this specification is built on

These come from the design discussion of 2026-11-06 and are binding for every
work item below. They are owner decisions, not implementation choices.

| # | Decision | Consequence for the work items |
|---|---|---|
| **DEC-1** | **Codec seam: add `plugins::IAudioDecoder`, do not extend `plugins::ICodec`.** `ICodec` is a published STABLE interface; changing `decode()`'s signature would break every downstream codec plugin. | A new interface is introduced (B-1). `ICodec` keeps its shape. The Opus module implements both. |
| **DEC-2** | **PCM callback: keep the old callback, add a new one.** The old `void(const float*, size_t)` callback stays and keeps working; a new callback carries time / channel / concealment information. | New SDK types (E-1 … E-4). The engine fires both, old callback first. |
| **DEC-3** | **Output format: follow the new requirement — decoder-native rate, reported explicitly.** The decoded PCM is delivered at the codec's native rate (48 kHz for Opus) and at the negotiated channel count; the new callback states the actual rate and channel count, so the host can resample/mix itself. No resampler module is built in this scope. | E-2 carries `sample_rate_hz` + `channels`. Rate conversion is explicitly out of scope (X-1). Stereo→mono downmix is a defined, rate-preserving operation (B-9). |
| **DEC-4** | **This round delivers the specification + design only.** Development plan and version planning are decided later. | Part G is dependency order only. No milestones, no version numbers, no release gates here. |

Two further design decisions are made by this document (they are
implementation-level, but they change the public shape and are therefore
recorded here):

| # | Decision | Rationale |
|---|---|---|
| **DEC-5** | **Two new plugin seams: `plugins::IAudioDecoder` (codec) and `plugins::IAudioReceiver` (pipeline).** The receiver pipeline lives in a new self-contained module `src/modules/audio_rx/`, not in the engine. | L4 says `src/engine/` only orchestrates plugins and must not include concrete module headers. Today the engine already reaches into `nimrtc/jb/jitter_buffer.hpp` and `nimrtc/opus/opus.hpp` (see `src/engine/src/engine_impl.hpp:91,120`). Putting the new pipeline behind a seam removes the need to add another concrete include and gives the pipeline a unit-testable home. |
| **DEC-6** | **Two new registry categories: `kAudioDecoder` and `kAudioReceiver`.** `EngineConfig` grows `audio_decoder_name` (default `"opus"`) and `audio_receiver_name` (default `"nimrtc_audio_rx"`). | The Opus plugin already registers under id `"opus"` in the `kCodec` category; ids are per-category, so the same id can serve the decoder slot without a second name. |

### Owner answers of 2026-11-06

The four design questions put to the owner on 2026-11-06 were answered as
follows, and the answers are binding:

| Question | Answer | Recorded as |
|---|---|---|
| 1. Codec seam: extend the published `plugins::ICodec`, or add a separate decoder interface? | **the latter** — a separate interface | **DEC-1** |
| 2. Which PCM callback shape does the host program against? | **the new callback** — the one carrying rate / channels / timestamps / concealment flags | **DEC-2**, E-1, E-2, E-9 |
| 3. What sample rate and channel count does the host receive? | **follow the new requirement** — the decoder's native rate, reported explicitly, no built-in resampler | **DEC-3**, A-2, A-5, X-1 |
| 4. What does this round deliver? | **the detailed specification and design first, including the P1 public SDK** — complete, so nothing is lost; the development plan and the version plan are decided afterwards | **DEC-4** |

Reading of answer 2: the new callback is the API the host is meant to use.
The legacy `set_on_audio_frame()` is kept because it is a published STABLE
symbol and cannot be removed without a MAJOR bump (ADR-015 §Decision 3),
but it is documented as legacy and is not the recommended surface.

Reading of answer 4: this document is the deliverable of this round. It
specifies every work item down to the interface level, including the full
P1 public SDK (Part E) and the verification for each item (Part V), so
that the later development plan can be derived from it without re-deriving
any design. It deliberately contains **no** schedule, no version numbers
and no release gates; Part G is dependency order only.

---

## 1. Current-state audit (verified facts)

Every line below was read in the tree at the baseline commit. Line numbers
are the ones cited in the design discussion and are given so a reviewer can
re-verify them.

### 1.1 Confirmed defects on the receive path

| ID | Fact | Where |
|---|---|---|
| F-1 | The Opus decoder is constructed but never called. `codec_plugin_` is only ever asked to `encode()`; `decode()` / `decode_plc()` are never reached from the engine. | `src/engine/src/engine_media.cpp:105` (construction), `src/engine/src/engine_media.cpp:520` (encode only) |
| F-2 | The receive path reinterprets the RTP payload as `float*` and hands it to the host as PCM. This is the Opus bit-stream presented as audio samples. | `src/engine/src/engine_timeline.cpp:264-266` |
| F-3 | The same payload is fed to the 3A render path as the AEC reference signal, so the echo canceller converges on noise. | `src/engine/src/engine_timeline.cpp:280-283` |
| F-4 | The documented contract is the opposite of the implementation: `on_audio_frame` is documented as "after SRTP decrypt + Opus decode + 3A post-process". | `docs/api/engine_api.md:177` |
| F-5 | Playout is arrival-driven, not clock-driven: `push` is immediately followed by `pop(now)` in the packet-reception path. The configured JB delay is therefore never really in effect, and inter-packet gaps are never detected — which is exactly why PLC cannot be hooked up. | `src/engine/src/engine_timeline.cpp:259-260` |
| F-6 | The audio jitter buffer is created without `clock_rate_hz`, so the RFC 3550 interarrival-jitter math falls back to 90 kHz — 2× wrong for 48 kHz Opus. | `src/engine/src/engine_timeline.cpp:253-257`, `src/modules/jb/src/jitter_buffer.cpp:159-160` |
| F-7 | The audio path uses the concrete `jb::JitterBuffer` directly; `EngineConfig::jb_name` ("adaptive") and the `plugins::IJB` seam are ignored for audio. | `src/engine/src/engine_impl.hpp:259` |
| F-8 | The demux treats every non-video payload type as audio and never negotiates which codec the peer uses; the remote `fmtp` is echoed back verbatim, never parsed. | `src/engine/src/engine_timeline.cpp:226-248`, `src/engine/src/engine_signaling.cpp:727-731` |
| F-9 | In-band FEC is inert: `opus_decode_float` is always called with `decode_fec = 0`, and `OPUS_SET_PACKET_LOSS_PERC` is never set. | `src/modules/opus/src/opus.cpp:305-306` |
| F-10 | `DecoderConfig` has no `packet_loss_perc` field, so redundancy cannot be configured even in principle. | `src/modules/opus/include/nimrtc/opus/opus.hpp:78-83` |
| F-11 | `Decoder::decode_plc` exists and is never called from the engine. | `src/modules/opus/src/opus.cpp:318` |

### 1.2 Confirmed defects on the send path

| ID | Fact | Where |
|---|---|---|
| F-12 | `send_audio()` uses function-local `static std::atomic` for sequence number, SSRC and RTP timestamp. Two engines in one process share all three — a direct violation of the "no global mutable state / multi-instance" architecture promise. | `src/engine/src/engine_media.cpp:533-535` |
| F-13 | The SSRC value `0xDEADBEEF` is hard-coded a second time in the SDP generator; the two copies must be kept in lockstep by hand. | `src/engine/src/engine_signaling.cpp:68-74` |
| F-14 | The engine never calls the RFC 7587 packetiser it ships. `opus::packetise()` / `opus::depacketise()` are implemented and unit-tested, but unused by the engine — one-frame packets happen to be wire-compatible, multi-frame packets are not reachable at all. | `src/modules/opus/include/nimrtc/opus/opus.hpp:276,297` |
| F-15 | The RTP timestamp advances by `num_samples`, i.e. by the *PCM* sample rate, while Opus RTP clocking is always 48 kHz (RFC 7587 §4.1). Correct only while `pcm_sample_rate_hz == 48000`. | `src/engine/src/engine_media.cpp:539` |
| F-16 | On encode failure the function returns before the timestamp is advanced, leaving a hole in the RTP timeline that the peer's jitter buffer sees as a loss. | `src/engine/src/engine_media.cpp:531` |
| F-17 | `opus::Encoder::encode` accepts only the six Opus frame sizes (120/240/480/960/1920/2880 samples per channel). A host pushing 441-sample or arbitrary blocks gets a silent zero-length result — the whole audio segment is dropped with no log and no error callback. | `src/modules/opus/src/opus.cpp:189` |
| F-18 | Channel count is self-contradictory: the SDP advertises `opus/48000/2` (stereo) while the encoder is created with `pcm_channels = 1` (mono). | `src/engine/include/nimrtc/engine/engine.hpp:138` vs `src/engine/src/engine_media.cpp:83` |
| F-19 | Encoder configuration is hard-coded in the engine (bitrate 64000, complexity 10, FEC on, DTX off, VAD off). `ICodec::update_config()` is implemented but never called, so nothing can be re-tuned at runtime. | `src/engine/src/engine_media.cpp:80-88` |

### 1.3 Missing capability (confirmed absent)

| ID | Gap | Where it would live |
|---|---|---|
| F-20 | No RTCP Receiver Report for audio. `rtp::RrBuilder` exists but is only used by tests. Chrome therefore has no loss/jitter feedback for our inbound audio. | `src/modules/rtp/include/nimrtc/rtp/packet.hpp:394` |
| F-21 | `Engine::Stats` has video counters only; nothing about audio is observable. | `src/engine/include/nimrtc/engine/engine.hpp:1142-1155` |
| F-22 | SDP carries no `a=ptime`, no `a=fmtp` for Opus by default, no `urn:ietf:params:rtp-hdrext:ssrc-audio-level` extension, and therefore no Chrome `audioLevel` statistic. | `src/engine/src/engine_signaling.cpp:40-58` |
| F-23 | No audio NACK and no RED/ULPFEC. Only Opus in-band FEC is (nominally) available — and it is disabled (F-9). | — |
| F-24 | No device layer. `src/plugins/include/nimrtc/plugins/` contains **no** `audio_source.hpp` / `audio_sink.hpp`, although `docs/plan/ohos-audio-hal.md` §3.1 designs those interfaces against files that do not exist. | `docs/plan/ohos-audio-hal.md:40-60` |
| F-25 | No A/V synchronisation. The audio callback carries no timestamp at all, so the host cannot align audio to video. | `src/engine/include/nimrtc/engine/engine.hpp:523` |
| F-26 | PCMU / PCMA are not implemented anywhere in the tree, although `AudioCodecConfig`'s doc comment still promises an "Opus-unavailable → PCMU" fallback. | `src/engine/include/nimrtc/engine/engine.hpp:124-126` |
| F-27 | There are no engine-level audio tests at all — only `src/modules/opus/tests/*`. The Chrome interop harness only proves the NimRTC → Chrome direction (`audioReceived` in `interop/chrome/test_chrome_opus.html` is set from Chrome's *inbound* stats). | `interop/chrome/test_chrome_opus.html:351-411` |

### 1.4 Corrections to the original issue list

Three statements in the original list do not survive verification and must
not be carried into the plan:

* ❗ *"The jitter buffer only adapts on the video side; audio uses a fixed
  delay."* — **False.** `jb::Config::adaptive` defaults to `true` and the
  engine does not override it, so the audio JB already uses
  `jb_strategy::AdaptiveDelayTracker`. The real audio problems are F-5
  (arrival-driven playout) and F-6 (wrong clock rate), both of which are
  cheaper to fix than building a new module.
* ❗ *"Opus is fixed at 64 kbps."* — **Incomplete.** The bitrate is not the
  problem; the problem is that the engine hard-codes every encoder knob
  (F-19) and never calls `update_config()`, so nothing can adapt even if a
  bandwidth estimate existed.
* ❗ *"`on_audio_frame` might be an intentional PCM passthrough debug mode."* —
  **False.** The documented contract is decode + 3A (F-4). This is a P0
  defect, not a debug feature.

---

## 2. Scope

### 2.1 In scope

* P0 — the receive path actually works: depacketise → decode → conceal →
  3A render → host callback, with a clock-driven playout.
* P0 — the send path stops lying: per-engine SSRC/sequence/timestamp,
  RFC 7587 framing, honest timestamps, no silent frame drops.
* P1 — the path becomes "like a real WebRTC stack": negotiated codec
  parameters, complete SDP, RTCP RR, audio statistics, and the public SDK
  described in Part E.
* P1 — the path becomes *observable*: Part L defines the mandatory log
  points of the audio path, their levels, their throttling and what must
  never be written.
* P2 — the quality and platform work (bitrate adaptation, DTX/VAD, device
  layer) specified at the same level, scheduled later.
* P2, moved into the v1.2.0 body — **A/V synchronisation inside the
  engine** (C-4) and the **G.711 module** (C-5). Both were P2 items in the
  first draft of this specification; the owner moved them forward on
  2026-11-10 (see §12, O-12 and O-13).

### 2.2 Out of scope for this specification

The exclusions are numbered `X-x`, so that they never collide with the
defect list (`F-x`, §1) or the verification list (`V-x`, Part V).

| # | Excluded | Why |
|---|---|---|
| X-1 | A resampler module. | DEC-3: the host is told the true rate and resamples. |
| X-2 | G.711 codecs beyond PCMU / PCMA (G.722, G.726, …). | C-5 implements the two mandatory WebRTC codecs (PCMU = payload type 0, PCMA = payload type 8, RFC 3551) and nothing else. A third G.7xx codec is a separate plan item. |
| X-3 | RED / ULPFEC. | Chrome does not use RED for audio in the profiles we interop with; Opus in-band FEC covers the first-order loss case. |
| X-4 | Audio transport-wide congestion control (TWCC / REMB for audio). | The scheduler already carries audio packets; the transport-cc seam itself is a video-plan item. |
| X-5 | Any change to the ICE / DTLS / SRTP layers. | — |
| X-6 | The 3A (AEC/ANS/AGC/VAD) implementation itself. | The 3A plugin seam already exists and works; only the *reference signal* fed to it is wrong (A-10). 3A quality tuning is C-2. |
| X-7 | Any change to the video pipeline. | Video is covered by `docs/plan/video-pipeline-v2-plan.md`; the two plans share the ADR-017 timeline but do not share work items. |

---

## 3. Architecture

### 3.1 Target data flow

```
                        ── SEND ────────────────────────────────────────────
host PCM (any block size)
   │
   ├─ engine::send_audio()
   │    ├─ 3A process_capture            (IAudio3A plugin)
   │    ├─ frame accumulator             (B-6: 20 ms @ 48 kHz slices)
   │    ├─ ICodec::encode                (encode direction, unchanged seam)
   │    ├─ opus::packetise()             (RFC 7587 framing, B-5)
   │    ├─ RTP header (per-engine SSRC/seq/ts, abs-send-time)
   │    ├─ SRTP protect → transport.send
   │
   └─ on encode failure: drop counter + on_error, timeline still advances

                        ── RECEIVE ─────────────────────────────────────────
SRTP unprotect → RTP parse
   │
   ├─ PT routing table (from the remote SDP)          (B-4)
   │     unknown / unsupported PT → error, packet dropped
   │
   ├─ IAudioReceiver::push(packet)                    (new module audio_rx)
   │    ├─ IJB::push (plugin seam, clock_rate_hz = 48000)   (B-3)
   │    ├─ timeline bookkeeping (RTP ts → expected sample count)
   │    └─ gap detection → PLC / FEC recovery queue    (B-7, B-8)
   │
   ├─ IAudioReceiver::tick(now)   ← driven by engine tick()   (B-2)
   │    ├─ IJB::pop(now)  (playout by clock, not by arrival)
   │    ├─ IAudioDecoder::decode_packet()  → N frames    (B-1)
   │    │     Opus impl: opus::depacketise() + opus::Decoder::decode()
   │    ├─ IAudioDecoder::decode_plc() for missing slots  (B-7)
   │    └─ FEC recovery via decode_fec=1 (opus keeps last packet)  (B-8)
   │
   ├─ engine: 3A process_render (AEC reference)          (B-9)
   │
   └─ host callbacks
        ├─ legacy: set_on_audio_frame(pcm, total_floats)   (E-2)
        └─ new:    set_on_audio_frame_info(info)            (E-2, E-4)
```

### 3.2 New and changed components

| Component | Kind | Location | Items |
|---|---|---|---|
| `plugins::IAudioDecoder` | new plugin interface | `src/plugins/include/nimrtc/plugins/audio_decoder.hpp` | B-1 |
| `plugins::IAudioReceiver` | new plugin interface | `src/plugins/include/nimrtc/plugins/audio_receiver.hpp` | B-2 |
| `nimrtc::audio_rx::Receiver` | new module implementation | `src/modules/audio_rx/` | B-2, B-7, B-8 |
| `nimrtc::opus::AudioDecoderPlugin` | new Opus-side implementation | `src/modules/opus/src/audio_decoder_plugin.cpp` | B-1, B-10 |
| `nimrtc::opus::Decoder` | extended concrete API | `src/modules/opus/include/nimrtc/opus/opus.hpp` | B-10 |
| `core::PluginRegistry` | two new slots | `src/core/include/nimrtc/core/registry.hpp` | B-1, B-2 |
| `engine::pal::resolve_audio_decoder` / `resolve_audio_receiver` | new resolvers | `src/engine/include/nimrtc/engine/pal/engine_plugin_resolver.hpp` | B-1, B-2 |
| `NimRTCEngine` | wiring + SDK | `src/engine/include/nimrtc/engine/engine.hpp`, `src/engine/src/*` | B-4, B-11, E-* |
| `Engine::Stats` | additive fields | `src/engine/include/nimrtc/engine/engine.hpp` | B-13, E-4, E-8 |

### 3.3 Threading model

* `engine::tick()` runs on the host's engine thread. All `IAudioReceiver`
  calls (`push`, `tick`) and all host audio callbacks happen on that thread.
  This matches the existing contract ("callbacks fire on the engine thread",
  `docs/api/engine_api.md:184`).
* The transport receive path may run on the transport thread. The existing
  engine already funnels `handle_rtp` work through the engine thread
  (the ICE callback path is serialized through the engine's rx queue); the
  audio receiver must keep that property. If the current implementation
  turns out to call `handle_rtp` on the transport thread, the receiver is
  given an internal mutex rather than relying on the caller — this is a
  verification task (V-2), not an assumption.
* `IAudioDecoder` and `IAudio3A` calls happen only on the engine thread.
  `IAudio3A` implementations must already be thread-safe (AEC reference
  path races with capture path) — that contract is unchanged.

---

## 4. Part A — P0 specification: make the receive path real

### A-1. Playout becomes clock-driven

The audio playout is driven by the engine's tick, not by packet arrival.

* `engine::tick()` calls `audio_receiver_->tick(now_us)` at a fixed place in
  the tick sequence — after the transport receive drain, before the SRTP
  send drain, so that a packet received in this tick can be played out in
  the same tick when its deadline has already passed.
* The receiver pops from the jitter buffer with the current time; the
  jitter buffer decides when a frame is due. The engine never calls
  `pop()` from the packet-reception path any more.
* **Tick-rate requirement:** the existing contract is "engine tick() must be
  called ≥ 30 Hz" (`src/engine/include/nimrtc/engine/engine.hpp:716-717`).
  For audio, a jitter buffer target delay of 40 ms with a 20 ms frame
  period needs a tick period well below the frame period. The specification
  therefore raises the **audio** requirement to **≥ 100 Hz** (10 ms) and
  documents it in `docs/api/engine_api.md`. This is a documentation and
  validation change, not a hard runtime limit: a slower tick degrades
  playout granularity but does not break the pipeline.
* If the host does not call `tick()` at all, the receiver produces no
  output — that is the intended consequence of a clock-driven design, and
  it is documented.

### A-2. Codec decoding on the receive path

* Every decoded frame carries, at minimum: PCM pointer, samples per
  channel, channel count, sample rate, the RTP timestamp of the frame, the
  frame duration in milliseconds, and the concealment / FEC-recovery flags.
* The decoded PCM is delivered **at the decoder's native rate** (48 kHz for
  Opus) and at the negotiated channel count (DEC-3).
* The engine delivers exactly what the decoder produced; it never
  resamples.

### A-3. Concealment (PLC) is driven by the timeline, not by decode failure

* A decode error is *not* the trigger for concealment. Concealment is
  triggered by **missing media time**: the receiver compares the expected
  RTP timestamp progression against what actually arrived and inserts
  concealment frames for the missing slots, so that the output cadence
  stays constant (A-4).
* The concealment frame duration is the negotiated frame duration
  (default 20 ms), and the number of concealed frames is derived from the
  timestamp gap.
* Consecutive concealed frames are capped (default 10 frames = 200 ms).
  Beyond the cap the receiver emits silence and increments a
  "concealment overflow" counter rather than generating an unbounded
  stretch of synthetic audio.

### A-4. Output cadence is constant

* For a stream with a 20 ms frame period, the receiver emits exactly
  20 ms of PCM per 20 ms of media time, whether the media was received,
  concealed, or FEC-recovered.
* This is the property that lets the host use a fixed-size ring buffer and
  lets the AEC reference stay aligned with the loudspeaker signal.

### A-5. Audio-level and channel handling on the receive path

* The receiver delivers the channel count the peer sent. Chrome negotiates
  Opus as `opus/48000/2` (stereo), so the decoder is configured for 2
  channels and the decoded frames are stereo.
* NimRTC's own capture side is mono by default (`pcm_channels = 1`). The
  downmix from the decoder's channel count to the configured output channel
  count is a defined, **rate-preserving** operation (average of channels
  for stereo→mono). It is performed by the receiver, and the reported
  `channels` in the new callback is the *post-downmix* count, i.e. the count
  the host actually receives.
* The downmix is not a resample: the sample rate is untouched (DEC-3).

### A-6. Send path: per-engine state

* SSRC, RTP sequence number and RTP timestamp move from function-local
  `static std::atomic` to per-engine members in `engine::Impl`.
* The SSRC is **randomised per engine** from a per-process seed, so two
  engines in one process never collide. The hard-coded `0xDEADBEEF` is
  removed from both the packet path and the SDP generator; the SDP
  generator reads the same member the packet path uses, so the two cannot
  drift again.
* For deterministic tests, the SSRC is injectable (an engine-config field
  or an internal setter) — a test must be able to assert an exact SSRC.

### A-7. Send path: framing and timestamps

* The engine accumulates caller PCM into 20 ms frames (960 samples per
  channel at 48 kHz) and encodes exactly one frame per RTP packet, using
  `opus::packetise()` so the wire format is RFC 7587 by construction
  rather than by luck.
* The RTP timestamp advances by the **encoded media duration in the 48 kHz
  RTP clock**, independent of whether the packet was actually emitted. A
  dropped frame leaves a gap in the sequence number but never a gap in the
  timestamp timeline (this is the difference between "loss" and "gap" for
  the receiver).
* If `pcm_sample_rate_hz != 48000`, the engine refuses to open with an
  explicit error rather than emitting a wrong clock (F-15). This is
  validated at `open()`.

### A-8. Send path: no silent audio loss

* An invalid frame size, an encoder error, or an empty buffer never
  disappears. Each occurrence increments a statistics counter and, at a
  throttled rate, fires `on_error` with a dedicated error code, so the
  host can see that its PCM rate is wrong instead of hearing silence.

### A-9. The receive path rejects what it cannot decode

* The remote SDP's audio `rtpmap` is parsed into a payload-type routing
  table: PT → encoding, clock rate, channels.
* A packet whose PT is not in the table is dropped with an error callback
  (`kAudioPayloadTypeUnknown`), not handed to the Opus decoder.
* A remote PT that maps to a codec NimRTC does not implement (PCMU, PCMA)
  produces a single, explicit "unsupported codec" error at negotiation
  time and disables the audio track — it never degrades into noise.

### A-10. The 3A render reference is the real decoded signal

* The AEC reference is the decoded, post-decode PCM that is about to be
  handed to the host — never the compressed payload (F-3).
* The `set_render_delivered()` call keeps its current position: after the
  host callback returns.

---

## 5. Part B — P1 specification: the interfaces, in detail

### B-1. `plugins::IAudioDecoder` — the codec seam

New file: `src/plugins/include/nimrtc/plugins/audio_decoder.hpp`
(interface only — L2: no implementation, no module dependency).

```cpp
namespace nimrtc::plugins {

/** Format the decoder is asked to produce. */
struct NIMRTC_API_STABLE AudioDecodeFormat {
    std::uint32_t sample_rate_hz   = 48000;  // decoder output rate (Opus: 48000)
    std::uint8_t  channels         = 1;      // 1 = mono, 2 = stereo
    std::uint8_t  payload_type     = 111;    // RFC 7587 Opus PT
    bool          use_inband_fec   = true;   // honour Opus in-band FEC
    bool          use_dtx          = false;  // peer signalled usedtx=1
    int           packet_loss_perc = 5;      // 0..100, in-band FEC redundancy
};

/** One decoded frame. PCM is interleaved, `samples_per_channel` per channel. */
struct NIMRTC_API_STABLE AudioDecodedFrame {
    const float*  pcm                = nullptr;
    std::size_t   samples_per_channel = 0;
    std::uint32_t sample_rate_hz      = 48000;
    std::uint8_t  channels            = 1;
    std::uint32_t rtp_timestamp       = 0;   // ts of the first sample
    std::uint16_t duration_ms         = 0;   // media duration of this frame
    bool          concealed           = false;// produced by PLC
    bool          fec_recovered       = false;// reconstructed from in-band FEC
};

class NIMRTC_API_STABLE IAudioDecoder : public IPlugin {
public:
    virtual AudioDecodeFormat format() const noexcept = 0;
    virtual Status update_format(AudioDecodeFormat f) noexcept = 0;

    /** Decode one complete RTP payload.
     *  The implementation owns the codec-specific RTP framing (RFC 7587
     *  for Opus), so one payload may yield several frames.
     *  @param out      caller-owned array of at least @p max_frames entries
     *  @return number of frames written (0 on error / malformed payload) */
    virtual std::size_t decode_packet(const std::uint8_t* payload,
                                      std::size_t  payload_len,
                                      std::uint32_t rtp_timestamp,
                                      AudioDecodedFrame* out,
                                      std::size_t  max_frames) noexcept = 0;

    /** Generate one concealment frame of @p duration_ms. */
    virtual std::size_t decode_plc(std::uint32_t rtp_timestamp,
                                   std::uint16_t duration_ms,
                                   AudioDecodedFrame* out,
                                   std::size_t  max_frames) noexcept = 0;

    virtual void reset() noexcept = 0;

    struct Stats {
        std::uint64_t frames_decoded      = 0;
        std::uint64_t bytes_decoded       = 0;
        std::uint64_t plc_frames          = 0;
        std::uint64_t fec_recovered_frames= 0;
        std::uint64_t decode_errors       = 0;
    };
    virtual Stats stats() const noexcept = 0;
};

class NIMRTC_API_STABLE IAudioDecoderFactory { /* id / display_name / create */ };
}
```

Contract notes:

* `decode_packet` is **payload-level**, not frame-level, on purpose: the
  RFC 7587 framing is Opus-specific and belongs inside the Opus
  implementation, not in the generic seam. This is what lets a future
  G.711 decoder reuse the same interface without inheriting Opus framing.
* `decode_packet` never conceals. Concealment is the pipeline's job (A-3).
* Not thread-safe — the engine serialises calls on the engine thread.
* `update_format` is called when the remote SDP changes the negotiated
  parameters; a decoder must be able to switch channel count / FEC
  behaviour without being recreated.

### B-2. `plugins::IAudioReceiver` — the pipeline seam

New file: `src/plugins/include/nimrtc/plugins/audio_receiver.hpp`.

```cpp
namespace nimrtc::plugins {

/** Per-stream configuration of the receive pipeline. */
struct NIMRTC_API_STABLE AudioReceiverConfig {
    std::uint32_t clock_rate_hz        = 48000;  // RTP clock of the remote stream
    std::uint32_t decoder_sample_rate_hz = 48000;
    std::uint8_t  decoder_channels      = 2;     // as negotiated
    std::uint8_t  output_channels       = 1;     // after downmix
    std::uint16_t frame_duration_ms     = 20;    // negotiated ptime
    std::uint16_t target_delay_ms       = 40;    // initial jitter buffer depth
    std::uint16_t min_delay_ms          = 10;
    std::uint16_t max_delay_ms          = 200;
    std::uint16_t max_concealed_frames  = 10;    // cap on consecutive PLC frames
    bool          enable_fec_recovery   = true;
    bool          enable_downmix        = true;
};

/** What the pipeline hands back to the engine. */
struct NIMRTC_API_STABLE AudioOutputFrame {
    const float*  pcm                 = nullptr;
    std::size_t   samples_per_channel = 0;
    std::uint32_t sample_rate_hz      = 48000;
    std::uint8_t  channels            = 1;
    std::uint32_t rtp_timestamp       = 0;
    std::int64_t  playout_time_us     = 0;  // engine-thread playout instant
    bool          concealed           = false;
    bool          fec_recovered       = false;
};

class NIMRTC_API_STABLE IAudioReceiver : public IPlugin {
public:
    using FrameCallback = std::function<void(const AudioOutputFrame&)>;
    using ErrorCallback = std::function<void(Status, std::string_view)>;

    virtual void set_callbacks(FrameCallback on_frame,
                                ErrorCallback on_error) noexcept = 0;

    /** Feed one inbound audio RTP packet (already SRTP-unprotected and
     *  parsed). `ssrc` identifies the stream. */
    virtual void push(const uint8_t* payload, std::size_t payload_len,
                      std::uint32_t rtp_timestamp, std::uint16_t seq,
                      std::uint32_t ssrc, TimestampUs recv_time_us) noexcept = 0;

    /** Advance the playout clock; emits whatever is due. */
    virtual void tick(TimestampUs now_us) noexcept = 0;

    /** Drop all state for a stream (stream ended / renegotiated). */
    virtual void reset_stream(std::uint32_t ssrc) noexcept = 0;

    struct Stats {
        std::uint64_t packets_received   = 0;
        std::uint64_t bytes_received     = 0;
        std::uint64_t frames_played_out  = 0;
        std::uint64_t frames_concealed   = 0;
        std::uint64_t frames_fec_recovered = 0;
        std::uint64_t packets_lost       = 0;
        std::uint64_t decode_errors      = 0;
        std::uint32_t current_delay_ms   = 0;
        std::uint32_t jitter_ms          = 0;
    };
    virtual Stats stats() const noexcept = 0;
};
```

The receiver owns: the jitter buffer instance for the stream, the decoder
instance, the timeline bookkeeping, concealment and FEC recovery. It does
**not** own 3A — the engine applies 3A render to the frames the receiver
emits (A-10), because 3A is already a plugin seam and the engine is the
component that owns it.

Implementation: new module `src/modules/audio_rx/`
(`include/nimrtc/audio_rx/audio_rx.hpp`, `src/`, `tests/`, own
`CMakeLists.txt` — L3). The module depends on `nimrtc::core` and
`nimrtc::plugins` only. The jitter buffer and the decoder are **injected**
as plugin interfaces (`IJB`, `IAudioDecoder`), so the module has no
dependency on `src/modules/jb` or `src/modules/opus` (L3: modules are
self-contained).

### B-3. Jitter buffer: use the seam, and use the right clock

* The receiver obtains its jitter buffer through `plugins::IJB` (the
  `"adaptive"` plugin, `EngineConfig::jb_name`), not through the concrete
  `jb::JitterBuffer`. This removes the concrete include from the engine
  (F-7) and lets the existing `jb_strategy` adaptive delay do its job.
* `IJB` gains the ability to be configured with the stream clock rate.
  `plugins::JBConfig` already has `clock_rate_hz`; the default `90000`
  must become `0` = "use the stream's clock rate", and the receiver always
  sets it to the negotiated rate (48000 for Opus). A `clock_rate_hz` of 0
  must never silently mean 90 kHz for an audio stream (F-6).
* `IJB::pop(now)` stays the playout trigger; the receiver calls it from
  `tick()` only.

### B-4. Payload-type routing and negotiation

* `process_remote_sdp()` parses the remote audio `rtpmap` into a table
  `PT → {encoding, clock_rate, channels}` stored in `engine::Impl`.
* The receive demux looks the PT up in that table (F-8). It no longer
  treats "not the video PT" as "audio".
* The remote `fmtp` is **parsed**, not echoed blindly. Recognised Opus
  parameters: `minptime`, `maxptime`, `useinbandfec`, `usedtx`,
  `stereo`, `sprop-stereo`, `sprop-maxcapturesize`.
* The answer SDP echoes the negotiated values back, but only the subset
  NimRTC actually supports.
* A remote PT whose encoding is not implemented produces
  `kAudioCodecUnsupported` at negotiation time (A-9).

### B-5. RFC 7587 framing on the send path

* The engine calls `opus::packetise()` with a single 20 ms frame per
  packet (code 0 framing), and the receiver calls `opus::depacketise()`
  inside the Opus decoder plugin (B-1).
* `ptime` is configurable (10 / 20 / 40 / 60 ms, matching the Opus frame
  sizes) through `EngineConfig::audio_ptime_ms`; multi-frame packets are
  produced when `ptime > 20 ms`, using code 1 / code 3 framing as
  appropriate.
* The SDP advertises the chosen `ptime` / `maxptime`.

### B-6. Frame accumulator on the send path

* `send_audio()` accepts any block size. The engine keeps a per-engine
  accumulator and emits frames of exactly `frame_duration_ms`.
* Residual samples stay in the accumulator across calls; a partial frame
  is never encoded.
* The accumulator is flushed (discarded) when the stream stops.

### B-7. Concealment in the receiver

* Timeline bookkeeping: for each stream the receiver tracks the RTP
  timestamp of the last emitted media and the expected frame duration.
  When the next packet's timestamp is ahead of the expectation, the missing
  slots are filled with concealment frames at the right positions.
* Concealment frames come from `IAudioDecoder::decode_plc()` and are
  flagged `concealed = true` in the output frame, so the host can render
  them differently (or drop them) if it wants.
* Consecutive concealment is capped (B-2, `max_concealed_frames`); beyond
  the cap the receiver emits silence and counts an overflow.

### B-8. In-band FEC recovery

* The Opus decoder plugin keeps the last successfully decoded payload. When
  a loss is detected and the *next* packet arrives, the decoder is asked
  to reconstruct the lost frame from the in-band FEC data of the newer
  packet (libopus `decode_fec = 1`).
* A FEC-recovered frame is emitted with `fec_recovered = true` in place of
  the concealment frame that was emitted for that slot — i.e. the receiver
  holds the concealment slot for one frame period before finalising it, so
  a FEC-recovered frame can replace it. This "one-frame lookahead" is the
  cost of FEC recovery and is specified explicitly: it adds one frame
  (20 ms) of latency to the recovered stream only.
* `OPUS_SET_PACKET_LOSS_PERC` is set from the negotiated redundancy
  (default 5 %, or the value derived from the observed loss rate).
* If FEC recovery does not produce a frame, the concealment frame stands.

### B-9. Downmix

* Stereo → mono downmix is a channel average applied by the receiver after
  decoding, before the 3A render reference and before the host callback.
* The downmix never changes the sample rate.
* If the negotiated channel count already equals the configured output
  channel count, the downmix is a no-op.

### B-10. Opus-side decoder extensions

`nimrtc::opus::Decoder` gains:

| Addition | Purpose |
|---|---|
| `DecoderConfig::packet_loss_perc` | redundancy hint → `OPUS_SET_PACKET_LOSS_PERC` |
| `DecoderConfig::use_inband_fec` | whether the decoder accepts FEC-recovered frames |
| `Decoder::decode_with_fec(data, len, out, cap)` | decode with `decode_fec = 1` (reconstructs the *previous* frame from this packet) |
| `Decoder::last_payload()` accessor | the receiver/decoder plugin keeps the previous payload for FEC recovery |
| `Decoder::set_channels(n)` / reconfigure on channel change | remote renegotiation from stereo to mono |

`nimrtc::opus::AudioDecoderPlugin` (new file
`src/modules/opus/src/audio_decoder_plugin.cpp`) implements
`plugins::IAudioDecoder` on top of these: it performs
`opus::depacketise()` on the payload, decodes each frame, and applies FEC
recovery when asked.

### B-11. Engine wiring

* `engine::Impl` gains: the audio receiver instance, the audio decoder
  instance, the PT routing table, per-engine audio send state (SSRC,
  sequence, timestamp), the frame accumulator, and the audio counters.
* `init_audio_modules()` resolves the decoder and the receiver through the
  PAL (`pal::resolve_audio_decoder`, `pal::resolve_audio_receiver`). The
  failure policy is specified in **B-16** — both are hard errors, and
  there is no fallback receiver.
* The receive path in `engine_timeline.cpp` stops touching PCM: it routes
  the packet to the receiver and returns. All PCM handling moves to the
  receiver's frame callback.

### B-12. RTCP Receiver Report for audio

* The engine sends an RR (RFC 3550 §6.4.2) with one report block per
  inbound audio SSRC, containing: fraction lost, cumulative packets lost,
  extended highest sequence number, interarrival jitter, and last SR.
* The values come from the receiver's statistics (B-2) plus the engine's
  inbound sequence tracking.
* Cadence: an RR is sent at least once per second while audio is inbound
  (RFC 3550 §6.4.2 recommends 5–10 s; WebRTC sends far more often because
  the browser's `inbound-rtp` statistics depend on it). The exact cadence
  is a tuning constant; 1 s is the initial value.
* The RR is sent through the existing SRTCP path.

### B-13. Audio statistics

`Engine::Stats` gains (all additive, all cumulative since `open()`):

| Field | Meaning |
|---|---|
| `audio_frames_sent` | PCM frames encoded and handed to the transport |
| `audio_packets_sent` | RTP packets emitted |
| `audio_bytes_sent` | RTP payload bytes emitted |
| `audio_frames_received` | decoded frames delivered to the host |
| `audio_samples_received` | samples per channel delivered to the host |
| `audio_concealed_frames` | PLC frames delivered to the host |
| `audio_fec_recovered_frames` | frames reconstructed from in-band FEC |
| `audio_packets_lost` | inbound packets inferred lost |
| `audio_decode_errors` | decoder errors |
| `audio_encode_errors` | encoder failures (invalid frame size, encoder error) |
| `audio_jitter_ms` | current interarrival jitter estimate |
| `audio_playout_delay_ms` | current playout delay |

### B-14. SDP completion

The offer and the answer both carry:

* `a=ptime:<n>` and `a=maxptime:<n>` matching the configured frame
  duration.
* `a=fmtp:111 minptime=10;useinbandfec=1;stereo=0;sprop-stereo=0`
  (parameters that reflect what NimRTC actually does — in-band FEC is
  enabled and used, so it must be signalled).
* `a=extmap:<id> urn:ietf:params:rtp-hdrext:ssrc-audio-level` so Chrome
  can report `audioLevel` in `inbound-rtp` (F-22). The extension id is
  negotiated per direction as with abs-send-time (ADR-017 pattern).
* The `a=ssrc:` line uses the per-engine SSRC (A-6), removing the
  duplicated constant.

### B-15. Registry, registration and build wiring

`DEC-6` introduces two registry categories. This item makes that concrete;
it is the piece that turns "two new seams" into "the seams are reachable
by id".

**`src/core/include/nimrtc/core/registry.hpp`** gains:

* Two forward declarations in the `nimrtc::plugins` block:
  `class NIMRTC_API_STABLE IAudioDecoderFactory;` and
  `class NIMRTC_API_STABLE IAudioReceiverFactory;`
* Two typed slots in `PluginRegistry`, each with the usual
  `register_*` / `get_*` / `list_*` triple:
  `TypedRegistry<plugins::IAudioDecoderFactory> audio_decoder_;` and
  `TypedRegistry<plugins::IAudioReceiverFactory> audio_receiver_;`
* Two `detail::Registrar::Category` enumerators: `kAudioDecoder` and
  `kAudioReceiver`, each with a matching `case` in the `Registrar`
  constructor's `switch`.
* Two registration macros, matching the existing style exactly:
  `NIMRTC_REGISTER_AUDIO_DECODER(id, factory_ptr)` and
  `NIMRTC_REGISTER_AUDIO_RECEIVER(id, factory_ptr)`.
* The back-compat shim at the bottom of the header gains
  `#include "nimrtc/plugins/audio_decoder.hpp"` and
  `#include "nimrtc/plugins/audio_receiver.hpp"`, so that the existing
  "include `<nimrtc/core/registry.hpp>` and you get everything" behaviour
  is preserved for the new seams.

**`src/engine/include/nimrtc/engine/pal/engine_plugin_resolver.hpp`** gains
`resolve_audio_decoder(std::string_view id)` and
`resolve_audio_receiver(std::string_view id)`, mirroring
`resolve_codec` / `resolve_audio3a` exactly (same lookup, same
`nullptr` contract, same doc-comment shape).

**`src/core/src/pal_default_registrars.cpp`** gains the
`nimrtc::audio_rx::register_default_plugins()` entry, and
`src/core/include/nimrtc/core/registry.hpp` gains the matching
`namespace audio_rx { void register_default_plugins() noexcept; }`
forward declaration, so that the unified
`core::register_all_default_plugins()` picks the new module up.

**`src/modules/audio_rx/`** (new module, L3) has its own
`CMakeLists.txt`, `include/nimrtc/audio_rx/audio_rx.hpp`, `src/`, and
`tests/`. It depends on `nimrtc::core` and `nimrtc::plugins` only — the
jitter buffer and the decoder are injected as plugin interfaces, so the
module has **no** dependency on `src/modules/jb` or `src/modules/opus`.

**The Opus module** (`src/modules/opus/`) registers its decoder under the
`kAudioDecoder` category with id `"opus"`. The existing `kCodec`
registration (id `"opus"`) is untouched — ids are per-category, so the two
slots coexist without a name clash. `nimrtc::opus::register_default_plugins()`
publishes both.

**`src/modules/opus/src/audio_decoder_plugin.cpp`** (new) implements
`plugins::IAudioDecoder` and is compiled into the existing
`nimrtc_opus` target — no new library.

**`src/engine/CMakeLists.txt`** gains a link dependency on
`nimrtc_audio_rx` so that the default receiver id `"nimrtc_audio_rx"`
resolves for consumers that link only the engine. This is a link-time
dependency, not an include: the engine only ever sees
`plugins::IAudioReceiver`.

### B-16. Engine-side plugin resolution and failure policy

* `init_audio_modules()` resolves the decoder first, then the receiver,
  in that order. The receiver is useless without a decoder.
* **Decoder resolution failure** (`kAudioReceiverMissing`'s sibling
  `kAudioDecoderMissing`) is a **hard error**: the audio track is
  disabled and `on_error` fires once. Audio is never silently faked —
  this is the P0 defect being fixed, and a fallback "pass-through"
  behaviour would reintroduce F-2 (bit-stream handed over as floats).
* **Receiver resolution failure** is also a hard error for the same
  reason: a missing receiver means the audio track is off, which is
  visible to the host. The spec deliberately does **not** provide a
  "built-in minimal receiver" fallback — a fallback that decoded
  nothing would be the old broken behaviour under a new name.
* When `NIMRTC_HAS_OPUS` is not defined, there is no decoder plugin at
  all. The engine then reports `kAudioDecoderUnsupported` once and the
  audio track stays off. This is the same contract the existing
  `AudioCodecConfig` doc comment promises for the PCMU fallback, except
  that the promise is now honest: NimRTC reports the gap instead of
  emitting a broken stream (F-26).
* `EngineConfig::audio_decoder_name = ""` disables the audio track
  explicitly. `EngineConfig::audio_receiver_name = ""` likewise.

### B-17. Cross-end capture timestamp on the receive path

`E-1` exposes `capture_time_us` in `AudioFrameInfo`, so the engine must
produce it. This item specifies where the number comes from; without it
the field would be a permanently-zero lie.

* The engine keeps, per inbound audio SSRC, the mapping
  `RTP timestamp → capture instant`, seeded from the peer's RTCP
  Sender Reports and the `abs-send-time` header extension (the
  cross-end timeline seam of ADR-017, which the video path already
  uses).
* When a frame's RTP timestamp is inside a known mapping interval, the
  engine interpolates the capture instant and fills
  `capture_time_us`.
* When no Sender Report or abs-send-time has been received yet, the field
  stays `0` and the documented meaning of `0` applies ("not yet
  reconstructible"). The engine does **not** substitute the local
  playout time — that would silently mix two different clocks.
* The legacy callback is unaffected; it carries no timestamp.
* This item is what makes C-4's A/V-sync contract possible on the
  receive side. C-4 remains the *host-side* alignment contract; the
  engine's job is only to hand over a comparable number.

---

## 6. Part C — P2 specification: quality and platform

These items are specified at the same level so that nothing is lost, but
they are not part of the P0/P1 critical path and their scheduling is
entirely up to the later development plan.

**Amendment of 2026-11-10 (owner decision).** Two of the five items left
the P2 line: **C-4 (A/V synchronisation)** and **C-5 (G.711)** are now
part of the v1.2.0 body, next to the P0/P1 work. C-4 is re-specified
below as an *engine-internal*, switchable feature; C-5 is specified at
the module level. C-1, C-2 and C-3 stay on the P2 line.

### C-1. Bitrate adaptation

* The bandwidth estimate (existing `plugins::IBwe`) is written back to the
  encoder through the existing `ICodec::update_config()` — no new seam is
  needed, the seam exists and is simply unused (F-19).
* `opus::Encoder::Impl::reconfigure` already supports `OPUS_SET_BITRATE`
  dynamically.
* A floor (e.g. 8 kbps) and a ramp rate (e.g. ≤ 10 %/s) are required so
  the encoder does not oscillate.

### C-2. DTX / VAD coupling

* The 3A plugin's VAD output (already exposed) drives the Opus DTX flag;
  DTX is only enabled when the peer signalled `usedtx=1`.
* Both directions must agree: DTX on one side only is acceptable but
  pointless, so the negotiation result governs.

### C-3. Device layer

* New plugin seams `plugins::IAudioSource` (capture) and
  `plugins::IAudioSink` (playback), following the shape already designed
  in `docs/plan/ohos-audio-hal.md` §3.1 (that document currently references
  files that do not exist — F-24; this item creates them).
* Platform implementations: WASAPI (Windows), CoreAudio (macOS), ALSA
  (Linux), OHAudio (OpenHarmony, per the decision already taken in
  `docs/plan/ohos-audio-hal.md` §1).
* The engine offers `start_audio()` / `stop_audio()` that drive the
  capture ring buffer and the playback sink; the host can still feed PCM
  manually (the existing `send_audio()` path stays).

### C-4. A/V synchronisation — inside the engine, switchable

> **Owner decision, 2026-11-10.** The alignment between the audio playout
> clock and the video capture timeline is done by **the engine**, not by
> the host, and it is a **configuration switch** — it can be turned on or
> off.

* **What the engine has.** Every inbound audio frame carries
  `capture_time_us` (B-17) and every inbound video frame's capture
  instant is reconstructible through the ADR-017 seam
  (`NimRTCEngine::capture_ts_us_for_ssrc()`). Both are *peer's* capture
  instants on the same Unix-microsecond axis, so the offset between the
  two streams is directly measurable.
* **What the engine computes.** The engine keeps a small ring of the most
  recent video capture instants (per inbound video SSRC). For every
  inbound audio frame with a reconstructible `capture_time_us` it picks
  the video frame whose capture instant is *closest* to the audio frame's
  (the two were captured together) and computes the skew
  `skew_us = audio.capture_time_us − video.capture_ts_us`.
  A skew of zero means "the audio frame and the video frame were captured
  at the same instant"; a non-zero skew means the two streams have
  drifted apart on the capture timeline.
* **What the engine does with it.** The skew becomes a **playout offset**
  for the audio receiver: the receiver delays (or, within its own bounds,
  advances) the audio playout so that audio and video captured together
  are presented together. The offset is smoothed (a simple IIR, α = 0.1)
  so that a single outlier packet cannot jump the playout, and it is
  clamped to ±`av_sync_max_skew_ms`.
* **The engine does not resample and does not re-time video.** Video
  timing stays where it is — the host renders the video frames it gets;
  only the audio playout is aligned. That keeps the item bounded and
  preserves DEC-3 (no built-in resampler) and X-7 (no change to the
  video pipeline). The video side's own render clock remains the video
  plan's item R3 (`v1.4.0`); it consumes the same skew later.
* **The seam.** The engine drives the receiver through the plugin
  interface and never through a concrete module header (L4), so the
  receiver gains exactly one method:

  ```cpp
  /** Adjust the playout target by `offset_ms` (positive = later).
   *  The receiver clamps the value to its own min/max delay. */
  virtual void set_playout_offset_ms(std::int32_t offset_ms) noexcept = 0;
  ```

  The receiver owns the playout clock; the engine owns the *decision* of
  what the offset should be.
* **The switch.** `EngineConfig::av_sync_enabled` (bool, default `true`)
  and `EngineConfig::av_sync_max_skew_ms` (int, default `100`). With the
  switch **off** the engine never touches the receiver's delay — the
  behaviour is exactly the one of `v1.1.x`, and the host aligns the
  streams itself with `capture_time_us` / `capture_ts_us`.
* **Degradation is explicit and never faked.** If there is no video
  track, if the capture instants are not yet reconstructible (0), or if
  the skew exceeds `av_sync_max_skew_ms`, the engine keeps the receiver's
  own delay, does **not** invent a capture instant, and reports the state
  through the counters `Stats::av_sync_skew_us` and
  `Stats::av_sync_frames_aligned` (B-13, E-8 amendment A-1).
* **The default is `true`, but it is inert without a video track.** With
  an audio-only session there is nothing to align against, so the switch
  has no effect and an audio-only host keeps its behaviour unchanged. A
  host that has both tracks and wants the old behaviour turns the switch
  off — one field, no rebuild.

### C-5. G.711 (PCMU / PCMA)

* New module `src/modules/g711/` (L3: own `CMakeLists.txt`, `include/`,
  `src/`, `tests/`) implementing `plugins::IAudioDecoder` (the receive
  path) and `plugins::ICodec` (the send path), so that the mandatory
  WebRTC codecs exist and F-26 is closed.
* **Registry ids.** The module registers under the ids `pcmu` and `pcma`
  in the `kAudioDecoder` and the `kCodec` category (ids are per category,
  DEC-6). `detail::decoder_id_for_encoding()` already lower-cases the
  `a=rtpmap:` encoding name, so the SDP name (`PCMU` / `PCMA`) maps onto
  the plugin id (`pcmu` / `pcma`) with no new mapping table — exactly the
  way `opus` already works.
* **Wire format.** RFC 3551: PCMU is payload type 0, PCMA is payload type
  8, both with a **8000 Hz** clock. One 20 ms frame is 160 samples. The
  codec is a stateless companding law (µ-law for PCMU, A-law for PCMA) —
  there is no encoder state, so the module needs no lookahead.
* **The clock-rate rule changes.** `pcm_sample_rate_hz` must equal the
  *negotiated codec's* clock rate: 48000 for Opus, 8000 for PCMU / PCMA.
  `kAudioClockMismatch` (Part D) is therefore reported when the PCM rate
  does not match the negotiated codec, not only when it differs from
  48000. A host that wants to send G.711 sets `pcm_sample_rate_hz = 8000`
  and `audio_codec.encoding = "PCMU"`.
* **No resampler.** DEC-3 stands: NimRTC does not resample. A host that
  captures at 48 kHz and wants to send G.711 must feed 8 kHz PCM.
* **SDP.** No `a=fmtp:` line is sent for G.711 (RFC 3551 defines no
  format parameters for PCMU / PCMA), and the Opus-only `a=fmtp` builder
  (`detail::build_local_opus_fmtp()`) must only run for Opus.
* **The stale promise goes away.** `AudioCodecConfig`'s doc comment
  ("falls back to the first statically-linked codec plugin (e.g. PCMU)")
  describes a fallback that never existed; with C-5 it is replaced by the
  explicit selection of `audio_decoder_name` / the negotiated encoding.

---

## 7. Part D — error handling

| Code | Constant | Meaning |
|---|---|---|
| `0x1A40` | `kAudioCodecUnsupported` | The negotiated audio codec / payload type is not implemented. |
| `0x1A41` | `kAudioDecodeFailed` | The decoder failed on a payload (malformed or corrupt). |
| `0x1A42` | `kAudioEncodeFailed` | The encoder rejected the PCM (invalid frame size, encoder error). |
| `0x1A43` | `kAudioReceiverMissing` | The audio receiver plugin could not be resolved. |
| `0x1A44` | `kAudioClockMismatch` | `pcm_sample_rate_hz` does not match the negotiated codec's RTP clock (Opus 48000, G.711 8000 — C-5). |
| `0x1A45` | `kAudioChannelMismatch` | Negotiated channel count could not be honoured. |
| `0x1A46` | `kAudioDecoderMissing` | The audio decoder plugin could not be resolved (B-16). |
| `0x1A47` | `kAudioDecoderUnsupported` | No decoder is available at all — e.g. `NIMRTC_HAS_OPUS` is off (B-16). |
| `0x1A48` | `kAudioPayloadTypeUnknown` | An inbound audio packet carried a payload type that is not in the negotiated routing table (B-4). |

The `0x1Axx` block is the audio range next to the existing `kAudio3A*`
codes (`0x1A00..0x1A1F`) and the video codes (`0x1A20..0x1A3F`); the new
values start at `0x1A40` and do not collide. Numeric values are stable per
the engine error-code compatibility rule.

`kAudioCodecUnsupported` (negotiation-time, "the peer offered a codec we
do not implement") and `kAudioPayloadTypeUnknown` (packet-time, "this
packet's PT is not in the table we negotiated") are deliberately
**different codes**: the first is a configuration condition that disables
the track, the second is a per-packet anomaly that is counted and dropped.
Collapsing them would make the two situations indistinguishable in the
statistics.

Error-reporting policy:

* Errors are reported through `on_error` (existing callback) and counted
  in `Engine::Stats`.
* Per-packet errors (decode failure of a corrupt packet) are **counted but
  not** reported individually — reporting every corrupt packet would flood
  the host. The first occurrence in a window and every N-th occurrence are
  reported (default: first + every 50th).
* Configuration errors (unsupported codec, clock mismatch) are reported
  once at negotiation / open time and disable the affected track.
* `kAudioDecoderMissing` / `kAudioDecoderUnsupported` /
  `kAudioReceiverMissing` are reported exactly once, at
  `open()`/`init_audio_modules()` time. They are not retried — the
  plugin set cannot change under a live engine.
* Every code in this table is also **logged** at the matching level
  (Part L, L-9): the host that installs no error callback still gets the
  record.

---

## 8. Part E — the P1 public SDK (the API surface)

This part is the contract the host application programs against. All new
symbols are **additive**; nothing existing changes shape (ADR-015,
STABLE-only-additive rule).

### E-1. New public types

```cpp
namespace nimrtc::engine {

/** Everything the host needs to know about one delivered audio frame. */
struct NIMRTC_API_STABLE AudioFrameInfo {
    /** Interleaved PCM, float, [-1.0, 1.0].
     *  Length = num_samples_per_channel * channels.
     *  Valid only for the duration of the callback. */
    const float*  pcm                 = nullptr;

    /** Samples per channel in `pcm`. */
    std::size_t   num_samples_per_channel = 0;

    /** Sample rate of `pcm` — the decoder's native rate (48000 for Opus).
     *  The host resamples if its device rate differs. */
    std::uint32_t sample_rate_hz      = 48000;

    /** Channel count of `pcm` — after downmix to the configured output
     *  channel count. */
    std::uint8_t  channels            = 1;

    /** RTP timestamp of the first sample in this frame. */
    std::uint32_t rtp_timestamp       = 0;

    /** Engine-thread instant at which the frame was handed to the host.
     *  On the engine's monotonic origin (same epoch as `tick()`'s `now_us`),
     *  NOT the Unix wall clock. */
    std::int64_t  playout_time_us     = 0;

    /** Peer's capture instant of this frame, in Unix microseconds.
     *  0 when the cross-end timeline seam has not yet reconstructed it
     *  (no usable Sender Report / abs-send-time yet — ADR-017).
     *  This is the audio side of the A/V-sync contract (C-4): the host
     *  compares it against the video frame's `capture_ts_us`. */
    std::int64_t  capture_time_us     = 0;

    /** True when this frame is concealment (PLC) audio. */
    bool          concealed           = false;

    /** True when this frame was reconstructed from in-band FEC. */
    bool          fec_recovered       = false;
};

/** The negotiated format of the inbound audio stream.
 *  Filled in by `remote_audio_format()` once the remote SDP has been
 *  processed. */
struct NIMRTC_API_STABLE AudioFormat {
    /** Decoder output rate — the codec's native rate (48000 for Opus). */
    std::uint32_t sample_rate_hz      = 48000;

    /** Channel count after the engine's downmix — what the host receives. */
    std::uint8_t  channels            = 1;

    /** The negotiated payload type (RFC 7587 Opus = 111). */
    std::uint8_t  payload_type        = 111;

    /** Negotiated frame duration in ms (10 / 20 / 40 / 60). */
    std::uint16_t frame_duration_ms   = 20;
};

using NIMRTC_API_STABLE AudioFrameInfoCallback =
    std::function<void(const AudioFrameInfo& info)>;

} // namespace nimrtc::engine
```

### E-2. New callback setter

```cpp
/** Install the detailed audio-frame callback.
 *  Fires on the engine thread, after decoding, after downmix, and after
 *  the 3A render stage.  When set, it is fired *after* the legacy
 *  callback.  Passing an empty function removes it. */
NIMRTC_API_STABLE void set_on_audio_frame_info(AudioFrameInfoCallback cb) noexcept;
```

The legacy callback keeps its exact signature and semantics:

```cpp
using NIMRTC_API_STABLE AudioFrameCallback =
    std::function<void(const float* pcm, size_t num_samples)>;
NIMRTC_API_STABLE void set_on_audio_frame(AudioFrameCallback cb) noexcept;
```

`num_samples` in the legacy callback is the **total number of interleaved
float samples** (`num_samples_per_channel * channels`), which is what the
current implementation already passes (F-2 path, `total_floats`). This is
documented explicitly in `docs/api/engine_api.md`, because it is the one
place where the legacy semantics were previously ambiguous.

Behaviour when both callbacks are installed: the legacy one fires first,
then the detailed one. Both receive the same buffer.

### E-3. New engine configuration fields

```cpp
struct NIMRTC_API_STABLE EngineConfig {
    /* ... existing fields ... */

    /** Audio decoder plugin id. Default "opus". */
    std::string_view audio_decoder_name = "opus";

    /** Audio receive pipeline plugin id. Default "nimrtc_audio_rx". */
    std::string_view audio_receiver_name = "nimrtc_audio_rx";

    /** Audio frame duration in ms. One of 10 / 20 / 40 / 60 (Opus sizes).
     *  Also the SDP ptime. Default 20. */
    std::uint16_t audio_ptime_ms = 20;

    /** Enable Opus in-band FEC. Must match the SDP signal we send. */
    bool audio_use_inband_fec = true;

    /** Enable Opus DTX (only honoured when the peer signals usedtx=1). */
    bool audio_use_dtx = false;

    /** Target jitter buffer depth for the audio stream, in ms. */
    int audio_jitter_target_delay_ms = 40;

    /** Maximum number of consecutive concealed frames. */
    int audio_max_concealed_frames = 10;

    /** Downmix the decoded stream to `pcm_channels`. */
    bool audio_downmix_to_pcm_channels = true;

    /** Enable the engine-internal A/V synchronisation (C-4).
     *
     *  When true (the default) the engine measures the skew between the
     *  audio capture timeline and the video capture timeline and adjusts
     *  the audio playout so that media captured together is presented
     *  together.  When false the engine never touches the receiver's
     *  playout delay and the host aligns the streams itself, using
     *  `capture_time_us` (audio) and `capture_ts_us` (video).
     *
     *  The switch is inert in an audio-only session: with no video track
     *  there is nothing to align against. */
    bool av_sync_enabled = true;

    /** Maximum skew the engine corrects, in milliseconds (default 100).
     *
     *  A skew beyond this bound is not corrected — the receiver keeps its
     *  own playout delay and the state is reported through the
     *  `av_sync_*` counters.  The bound exists so that a peer whose
     *  clocks are wrong cannot make the engine chase it. */
    int av_sync_max_skew_ms = 100;
};
```

`pcm_sample_rate_hz` keeps its meaning (the rate of the PCM the host
feeds *and* receives). It must equal the **negotiated codec's** clock
rate: 48000 for Opus, 8000 for PCMU / PCMA (F-15 / `kAudioClockMismatch`
/ C-5).

### E-4. New engine accessors

```cpp
/** Live inbound+outbound audio statistics.  The existing `Engine::Stats`
 *  struct gains the audio counters listed in B-13; this accessor keeps
 *  its existing name and its existing `NIMRTC_API_UNSTABLE` marker
 *  (`src/engine/include/nimrtc/engine/engine.hpp:1159`). */
NIMRTC_API_UNSTABLE Stats stats() const noexcept;

/** The negotiated remote audio format, once the remote SDP has been
 *  processed.  Returns false when no audio track is negotiated yet, in
 *  which case `out` is left untouched. */
NIMRTC_API_STABLE bool remote_audio_format(AudioFormat* out) const noexcept;
```

`Stats` is deliberately **not** promoted to STABLE here. It already
carries `NIMRTC_API_UNSTABLE` and ADR-015 §Decision 2 allows an UNSTABLE
symbol to change in any MINOR bump; promoting it would be an unrelated
policy decision and is not part of this work.

### E-5. Where the new symbols live

The SDK change touches exactly one public header plus two new plugin
headers. The engine's public header is
`src/engine/include/nimrtc/engine/engine.hpp`.

| Symbol | Header | Location |
|---|---|---|
| `AudioFrameInfo`, `AudioFormat`, `AudioFrameInfoCallback` | `engine.hpp` | in the "Public callbacks" block, next to `AudioFrameCallback` (`:523`) |
| `set_on_audio_frame_info()` | `engine.hpp` | next to `set_on_audio_frame()` (`:600`) |
| `EngineConfig` new fields | `engine.hpp` | inside `struct EngineConfig` (`:145`), appended at the **end** of the struct |
| `Stats` new fields | `engine.hpp` | inside `struct Stats` (`:1142`), appended at the **end** of the struct |
| `remote_audio_format()` | `engine.hpp` | in the accessor block, next to `stats()` (`:1159`) |
| `plugins::IAudioDecoder` + `AudioDecodeFormat` + `AudioDecodedFrame` | `src/plugins/include/nimrtc/plugins/audio_decoder.hpp` | **new file** |
| `plugins::IAudioReceiver` + `AudioReceiverConfig` + `AudioOutputFrame` | `src/plugins/include/nimrtc/plugins/audio_receiver.hpp` | **new file** |

Two rules that are part of the specification, not implementation taste:

* **Append-only inside a published struct.** `EngineConfig` and `Stats`
  are published STABLE structs. New members go at the end, never in the
  middle, so that the existing initialiser order and any aggregate
  initialisation in downstream code keeps compiling. No existing member is
  reordered, removed, or given a new default.
* **Every new public symbol carries a stability marker**, and every new
  public header `#include`s `<nimrtc/core/api_version.hpp>`. The CI gates
  `scripts/api_freeze_scan.js` (per-symbol classification) and
  `scripts/check_api_marker_includes.py` (include hygiene) enforce this on
  all four platforms.

### E-6. ABI / compatibility note on `EngineConfig`

`EngineConfig` is passed **by value** to the `NimRTCEngine` constructor
(`engine.hpp:556`). Adding fields therefore changes the size of a type
that crosses the header boundary.

* The change is **source-compatible**: every existing caller compiles
  unchanged, because the new members have default initialisers and are
  appended at the end.
* It is **not ABI-compatible** for a caller that was compiled against the
  old header and is not recompiled. `nimrtc_engine` is a **static**
  library (`src/engine/CMakeLists.txt:16`, `add_library(nimrtc_engine
  STATIC …)`), so there is no separate DLL to mismatch against; the
  consumer links the library and its own translation units together, and
  a mismatch cannot arise at run time. Every consumer rebuilds against
  the headers it uses.
* Per ADR-015 §Decision 4, ABI is committed only within a `v1.0.x` patch
  line. A MINOR bump is the honest signal for a change of this shape, and
  the release that carries it must therefore be a MINOR (or MAJOR) bump —
  never a patch release. The version decision itself belongs to the
  version plan (DEC-4); this paragraph only records the constraint that
  the plan must satisfy.

The same reasoning applies to `Stats`: it is returned by value, its new
fields are appended, and it stays `NIMRTC_API_UNSTABLE` (E-4).

### E-7. Documentation deliverables of the SDK change

* `docs/api/engine_api.md` — callback table updated with
  `on_audio_frame_info`, the legacy `num_samples` semantics spelled out,
  the tick-rate requirement raised to ≥ 100 Hz for audio, and the
  "decode + 3A" contract corrected (F-4).
* `docs/api/engine_config.md` — the new configuration fields.
* `CHANGELOG.md` — the new SDK entry.

### E-8. Complete inventory of new public symbols

This is the exhaustive list of every public symbol the P1 SDK change
adds. Nothing may be added beyond this list without a spec amendment.

**`src/engine/include/nimrtc/engine/engine.hpp`** — 7 new symbols:

| # | Symbol | Kind | Marker | Notes |
|---|---|---|---|---|
| 1 | `nimrtc::engine::AudioFrameInfo` | struct | STABLE | E-1. 9 fields, all with default initialisers. |
| 2 | `nimrtc::engine::AudioFormat` | struct | STABLE | E-1. 4 fields. |
| 3 | `nimrtc::engine::AudioFrameInfoCallback` | type alias | STABLE | E-1. `std::function<void(const AudioFrameInfo&)>` |
| 4 | `NimRTCEngine::set_on_audio_frame_info()` | member fn | STABLE | E-2. |
| 5 | `NimRTCEngine::remote_audio_format()` | member fn | STABLE | E-4. |
| 6 | `EngineConfig` + 8 new members | struct members | STABLE (parent) | E-3. Appended at the end of the struct. |
| 7 | `Stats` + 12 new members | struct members | UNSTABLE (parent) | B-13. Appended at the end of the struct. |

**`src/plugins/include/nimrtc/plugins/audio_decoder.hpp`** (new) — 4 new symbols:

| # | Symbol | Kind | Marker |
|---|---|---|---|
| 1 | `nimrtc::plugins::AudioDecodeFormat` | struct | STABLE |
| 2 | `nimrtc::plugins::AudioDecodedFrame` | struct | STABLE |
| 3 | `nimrtc::plugins::IAudioDecoder` | class | STABLE |
| 4 | `nimrtc::plugins::IAudioDecoderFactory` | class | STABLE |

**`src/plugins/include/nimrtc/plugins/audio_receiver.hpp`** (new) — 4 new symbols:

| # | Symbol | Kind | Marker |
|---|---|---|---|
| 1 | `nimrtc::plugins::AudioReceiverConfig` | struct | STABLE |
| 2 | `nimrtc::plugins::AudioOutputFrame` | struct | STABLE |
| 3 | `nimrtc::plugins::IAudioReceiver` | class | STABLE |
| 4 | `nimrtc::plugins::IAudioReceiverFactory` | class | STABLE |

**`src/core/include/nimrtc/core/registry.hpp`** — 6 new symbols, all in
`nimrtc::core::PluginRegistry` (the class is already STABLE; the new
members are additions to it, so they inherit the parent's marker):

`register_audio_decoder()`, `get_audio_decoder()`,
`list_audio_decoders()`, `register_audio_receiver()`,
`get_audio_receiver()`, `list_audio_receivers()`.

Plus two non-public registration macros
(`NIMRTC_REGISTER_AUDIO_DECODER`, `NIMRTC_REGISTER_AUDIO_RECEIVER`) —
macros are not symbols and are not scanned by the API-freeze gate.

**Total: 21 new public symbols across 4 headers (2 of them new).**

**Public-header count.** `api_freeze_scan.js` collects every
`src/<mod>/include/nimrtc/**/*.hpp`, so the new module header
`src/modules/audio_rx/include/nimrtc/audio_rx/audio_rx.hpp` is counted as
well. The count therefore goes **106 → 109**: two plugin headers plus one
module header. Every symbol in all three headers carries a marker, so the
scan's `Missing` count stays at 0. The scan is a *gate on missing markers*,
not a gate on the header count, so no baseline file needs updating — but
the number is recorded here so a reviewer can verify the delta.

#### Amendment A-1 (2026-11-10) — the symbols added by C-4, C-5 and Part L

The inventory above is the P1 body. The 2026-11-10 owner decision added
three things to the v1.2.0 scope — the engine-internal A/V
synchronisation (C-4), the G.711 module (C-5) and the logging contract
(Part L) — and each of them touches the public surface. This amendment
extends the inventory; it does not replace it. The rule "nothing may be
added beyond this list without a spec amendment" still holds, and this is
that amendment.

| # | Symbol | Kind | Marker | Notes |
|---|---|---|---|---|
| 1 | `plugins::IAudioReceiver::set_playout_offset_ms()` | member fn | STABLE (parent) | C-4. One method on the published receiver seam; the receiver clamps the value to its own delay bounds. |
| 2 | `EngineConfig::av_sync_enabled` | struct member | STABLE (parent) | C-4 / E-3. Appended at the end of the struct. |
| 3 | `EngineConfig::av_sync_max_skew_ms` | struct member | STABLE (parent) | C-4 / E-3. Appended at the end of the struct. |
| 4 | `Stats::av_sync_skew_us` | struct member | UNSTABLE (parent) | C-4. Appended at the end of the struct. |
| 5 | `Stats::av_sync_frames_aligned` | struct member | UNSTABLE (parent) | C-4. Appended at the end of the struct. |

Plus one **new public header**:

| Header | Symbol | Kind | Marker | Notes |
|---|---|---|---|---|
| `src/modules/g711/include/nimrtc/g711/g711.hpp` | `register_default_plugins` | function | STABLE | C-5. Same shape as the `audio_rx` module's entry point. |

**Part L adds no public symbol at all.** The logging contract is built on
`core::log::Logger`, which is already published and already STABLE
(`src/core/include/nimrtc/core/log.hpp`); Part L fixes *which* lines are
written, at which level, with which fields — it does not add a knob.

**Totals after amendment A-1: 27 new public symbols across 5 headers
(3 of them new).** The public-header count goes **106 → 110**: the two
plugin headers, the `audio_rx` module header and the new `g711` module
header.

### E-9. Host migration guide

The P1 SDK is additive; an existing host keeps compiling and keeps
working. The guide below tells a host what it *gains* and what it *must
change*.

**Nothing must change.** A host that only uses `set_on_audio_frame()`
and `send_audio()` continues to work unchanged. The legacy callback now
receives genuinely decoded PCM (it previously received the Opus bit-stream
mislabelled as floats — F-2), so a host that was "working" against the
broken build was in fact processing garbage; after this change it
processes real audio. That is a **behavioural fix, not a regression**,
but it is worth calling out in the CHANGELOG because the output changes.

**Recommended migration** — install the new callback and keep the old one
only if a legacy consumer still needs it:

```cpp
NimRTCEngine engine(cfg);

engine.set_on_audio_frame([](const float* pcm, size_t n) {
    // Legacy: n = total interleaved floats (samples_per_channel * channels).
    legacy_sink(pcm, n);
});

engine.set_on_audio_frame_info([](const nimrtc::engine::AudioFrameInfo& f) {
    // f.pcm is valid only for the duration of this call.
    // f.sample_rate_hz / f.channels tell you what you actually got.
    // f.concealed / f.fec_recovered let you distinguish synthetic audio.
    modern_sink(f.pcm, f.num_samples_per_channel * f.channels,
                f.sample_rate_hz, f.channels, f.rtp_timestamp);
});
```

**Reading the negotiated format** — a host that must know the rate before
the first frame arrives (e.g. to pre-allocate a resampler) asks the
engine:

```cpp
nimrtc::engine::AudioFormat fmt{};
if (engine.remote_audio_format(&fmt)) {
    // fmt.sample_rate_hz, fmt.channels, fmt.payload_type,
    // fmt.frame_duration_ms are negotiated.
}
```

**Buffer lifetime** — `f.pcm` points into a decoder-owned buffer that is
reused for the next frame. A host that needs to keep the audio must copy
it. This matches the existing contract of the legacy callback and is
stated explicitly because it is the most common integration mistake.

**Tick rate** — the host must call `tick()` at ≥ 100 Hz for audio
playout to be smooth (A-1). Hosts that call it at 30 Hz still get
continuous audio, with coarser granularity.

**Backwards compatibility rules the implementation must honour:**

* The legacy callback fires **before** the new one, with the same buffer
  (E-2).
* Installing the new callback does **not** uninstall the legacy one, and
  vice versa. Both may be absent.
* When neither is installed, the receiver still runs (decoding happens
  regardless) — the frames are simply dropped. The engine must not skip
  decoding because nobody is listening, because the statistics (B-13) and
  the 3A render reference (A-10) depend on it.

---

## 9. Part L — logging and observability

> **Why this part exists.** The audio pipeline fails silently by
> nature: a frame that never arrived looks exactly like a frame that
> arrived and was played out, and a peer that sends a codec we cannot
> decode looks exactly like silence. The tree already has a logger
> (`core::log::Logger` — the one global singleton L6 allows) and the
> engine uses it in a handful of places, but nothing said *which* log
> points the audio path owes the host, at which level, with which
> fields, and what must never be written. This part turns the log into a
> contract.

### L-1. One logger, no second one

Every log line in the audio path goes through `core::log::Logger`
(`src/core/include/nimrtc/core/log.hpp`), through the `NIMRTC_LOG_*`
macros or the free functions. No module writes to `stderr` / `stdout`
directly, no module owns a second logger, and no module introduces a
global of its own (L6). The logger is the *only* global mutable state
the audio path may touch.

### L-2. The mandatory log points

The following log points are mandatory. "Mandatory" means: a reviewer
may reject a change that removes one of them without an amendment, and a
test asserts that it exists (L-11, V-1).

| # | Where | Level | Message | Fields |
|---|---|---|---|---|
| L-2.1 | engine — module resolution | Info / Error | `audio: decoder plugin resolved` / `audio: decoder plugin unavailable` | plugin id, error code |
| L-2.2 | engine — module resolution | Info / Error | `audio: receiver plugin resolved` / `audio: receiver plugin unavailable` | plugin id, error code |
| L-2.3 | engine — negotiation | Info | `audio: negotiated codec` | encoding, payload type, clock rate, channels |
| L-2.4 | engine — PT routing | Debug | `audio: payload-type routing table` | audio PT, video PT |
| L-2.5 | engine — A/V sync | Debug | `audio: av-sync offset` | skew µs, offset ms, aligned frames |
| L-2.6 | `audio_rx` — lifecycle | Info | `audio-rx: pipeline started` / `audio-rx: pipeline stopped` | ssrc, rate, channels, ptime |
| L-2.7 | `audio_rx` — playout | Debug | `audio-rx: playout delay` | delay ms, jitter ms |
| L-2.8 | `audio_rx` — concealment | Warn | `audio-rx: concealment` | consecutive frames, lost packets |
| L-2.9 | `audio_rx` — FEC recovery | Debug | `audio-rx: fec recovered` | RTP timestamp |
| L-2.10 | `audio_rx` — decode error | Warn | `audio-rx: decode failed` | error code, payload length (never the payload itself — L-6) |
| L-2.11 | `opus` / `g711` module | Warn / Error | `opus: encode failed` / `g711: decode failed` | codec, error code |
| L-2.12 | engine — send path | Warn | `audio: send dropped` | reason (encoder error / empty buffer) |

### L-3. What belongs to which level

| Level | Use |
|---|---|
| Trace | One line per frame or per packet. Never on by default (L-4). |
| Debug | State transitions: delay changes, routing tables, recoveries. |
| Info | Lifecycle: modules resolved, codec negotiated, pipeline started/stopped. |
| Warn | A degradation the host should know about that does not stop the stream: concealment, a dropped send, a corrected skew. |
| Error | A hard failure of a unit: no decoder, no receiver, an unrecoverable decode error. |
| Critical | The engine itself cannot continue. |

The level of a mandatory point is fixed by the table in L-2. A point may
additionally be logged at a lower level, but never at a higher one: a
concealment logged as `Error` would train the host to ignore `Error`.

### L-4. Per-frame logging is opt-in

At the default level (`Info`) the audio path writes **no per-frame and no
per-packet line**. One `Trace` line per frame is the maximum a frame
produces; a frame never produces two lines at the same level. This is
what keeps the log usable: an audio stream produces 50 frames per second
per direction, and a log that carries one line per frame is a log nobody
reads.

### L-5. Anomalies are throttled

A recurring anomaly is reported **on the first occurrence and every 50th
one** — the same rule the error-reporting policy of Part D uses for
per-packet errors. The counter that drives the throttling is the same
counter that feeds `Stats`, so the log and the statistics can never
disagree about how often something happened.

### L-6. Nothing sensitive, nothing bulky

A log line carries **metadata only**:

* never PCM samples, never a PCM buffer's content, never a payload dump;
* never key material — no ICE ufrag/pwd, no DTLS or SRTP keys, no
  certificate content;
* never a whole SDP (the negotiated parameters are logged, the whole
  description is not);
* only counts, timestamps, SSRCs, payload types, codec names, delay and
  skew values.

### L-7. Logging must not change the behaviour of the media path

* Every log call is `noexcept`; a sink that throws is swallowed.
* The **string is only built when the level is enabled.** The
  `NIMRTC_LOG_*` macros format unconditionally, so a call on a per-frame
  path must be guarded:

  ```cpp
  if (core::log::Logger::instance().level() <= core::log::Level::Trace) {
      NIMRTC_LOG_TRACE("frame ts=" << ts << " samples=" << n);
  }
  ```

  A per-frame log call without the guard is a defect, not a style
  question: the formatting cost is paid on every frame even when the
  line is thrown away.
* A disabled or failing sink cannot affect the media path. Logging is
  never on the critical path of a deadline.

### L-8. One prefix per component

Every line starts with the prefix of the component that wrote it:

| Prefix | Component |
|---|---|
| `audio:` | the engine (`src/engine/`) |
| `audio-rx:` | the `audio_rx` module (`src/modules/audio_rx/`) |
| `opus:` | the Opus module (`src/modules/opus/`) |
| `g711:` | the G.711 module (`src/modules/g711/`) |

A module never logs another module's business: the engine does not log
"the decoder failed", it logs "the decoder plugin is unavailable" and
leaves the decode failure to the decoder's own line. A host filters the
log with one substring match.

### L-9. The log is the human-readable face of the counters

Two rules, both directions:

* every anomaly that is counted in `Engine::Stats` produces a log line at
  the matching level;
* every `Warn` / `Error` line corresponds to a counter in `Engine::Stats`.

A degradation that is invisible in the statistics and visible in the log
is a defect, and so is the reverse. The host therefore has two views of
the same fact — one for humans, one for machines.

### L-10. What Part L does not add

* No new public symbol (see E-8, amendment A-1).
* No metrics exporter, no log-to-file contract, no per-host log format.
  The sink is pluggable (`core::log::Sink`), which is where a host that
  wants JSON or a file plugs in — that is the host's code, not ours.
* No change to the `on_error` callback: the callback is the
  *programmatic* contract, the log is the *diagnostic* one. Neither
  replaces the other.

### L-11. The verification of the log surface

The log surface is verified by tests, not by review:

* a test installs a capturing `core::log::Sink` and asserts that an
  induced decode failure produces exactly one `Warn` line, that the line
  carries the `audio-rx:` prefix, and that no PCM byte appears in it;
* a test asserts that at the default level a running audio session
  produces no per-frame line;
* the guard of L-7 (a per-frame log call without the level check) is
  reviewed in the same pass as the seam changes.

### L-12. The documentation

`docs/api/engine_api.md` gains a section describing the audio log
surface — the prefixes, the levels, the mandatory points and the
throttling rule; `docs/zh/architecture.md` mirrors the three
architectural decisions (ADR-012). A host that reads only the API
reference must be able to find out which lines it can expect.

---

## 10. Part V — verification

### V-1. Unit tests

| Test | Location | Asserts |
|---|---|---|
| Opus decoder plugin round-trip | `src/modules/opus/tests/test_opus_audio_decoder.cpp` | encode → RTP payload → `decode_packet` yields PCM of the expected length |
| Depacketise of all RFC 7587 codes | `src/modules/opus/tests/test_opus_packetise.cpp` (extend) | code 0/1/2/3 round-trip, malformed payloads rejected |
| PLC frame properties | `src/modules/opus/tests/test_opus.cpp` (extend) | `decode_plc` yields the requested duration and is flagged |
| FEC recovery | `src/modules/opus/tests/test_opus_audio_decoder.cpp` | dropping a packet and decoding the next with FEC yields a `fec_recovered` frame |
| Receiver timeline | `src/modules/audio_rx/tests/test_audio_rx.cpp` | a timestamp gap produces exactly the missing number of concealed frames |
| Receiver cadence | `src/modules/audio_rx/tests/test_audio_rx.cpp` | output sample count == media time × rate, received + concealed |
| Receiver downmix | `src/modules/audio_rx/tests/test_audio_rx.cpp` | stereo→mono halves the channel count, preserves the sample count |
| Sender framing | `src/modules/audio_rx/tests/test_audio_rx.cpp` or engine test | `send_audio` with a non-frame-sized block yields one packet per 20 ms and a continuous timestamp |
| Two engines, one process | `tests/test_engine_audio_state.cpp` | two engines have different SSRCs and independent sequence/timestamp progressions (F-12) |
| PT routing | `tests/test_engine_audio_state.cpp` | an unknown PT is rejected with `kAudioPayloadTypeUnknown`, not decoded |
| Legacy callback semantics | `tests/test_engine_audio_state.cpp` | the legacy callback's `num_samples` equals `samples_per_channel * channels` of the new callback's frame (E-2 / O-2) |
| New callback flags | `tests/test_engine_audio_state.cpp` | a concealed frame has `concealed == true`; a FEC-recovered frame has `fec_recovered == true`; neither is ever set on a normally received frame |
| Registry slots | `tests/test_engine_audio_state.cpp` | `get_audio_decoder("opus")` and `get_audio_receiver("nimrtc_audio_rx")` resolve after `register_all_default_plugins()` (B-15) |
| Missing-receiver failure | `tests/test_engine_audio_state.cpp` | an unknown `audio_receiver_name` produces `kAudioReceiverMissing` exactly once and disables the audio track (B-16) |
| Negotiated format accessor | `tests/test_engine_audio_state.cpp` | `remote_audio_format()` returns false before the remote SDP is processed, and true with the negotiated values afterwards (E-4) |
| SDP contents | `tests/test_engine_audio_state.cpp` | the offer carries `a=ptime`, `a=maxptime`, the Opus `a=fmtp`, the audio-level `a=extmap`, and the per-engine SSRC (B-14) |
| Encoder error reporting | `tests/test_engine_audio_state.cpp` | an invalid PCM block size increments `audio_encode_errors` and fires `on_error` — it is never silently dropped (A-8) |
| Capture timestamp | `tests/test_engine_audio_state.cpp` | with no Sender Report received, `capture_time_us == 0`; it is never faked from the local playout clock (B-17) |
| G.711 round-trip | `src/modules/g711/tests/test_g711.cpp` | the µ-law / A-law round-trip is sample-exact (both companding laws are lossless integer mappings), and a 20 ms frame yields exactly 160 samples at 8 kHz (RFC 3551, C-5) |
| G.711 negotiation | `tests/test_engine_audio_state.cpp` | a peer that offers PCMU is answered with `a=rtpmap:0 PCMU/8000`; `pcm_sample_rate_hz == 8000` is accepted and `48000` is rejected with `kAudioClockMismatch` (C-5) |
| A/V sync offset | `tests/test_engine_audio_state.cpp` | with a synthetic skew between the audio and the video capture timelines, the engine computes the skew, clamps it at `av_sync_max_skew_ms`, and hands the offset to the receiver through `set_playout_offset_ms()` (C-4) |
| A/V sync switch | `tests/test_engine_audio_state.cpp` | with `av_sync_enabled == false` the receiver's playout delay is never touched; with no video track the switch has no effect (C-4) |
| Log surface | `tests/test_engine_audio_log.cpp` | an induced decode failure produces exactly one `Warn` line carrying the `audio-rx:` prefix and no PCM byte; at the default level a running session produces no per-frame line (L-11, L-4) |

### V-2. Engine integration tests

* `tests/test_engine_audio_loopback.cpp` — two engines over the loopback
  transport: engine A sends PCM, engine B decodes it and the received PCM
  matches the sent PCM within a tolerance (lossy codec → compare energy
  envelope, not sample equality).
* Loss injection — drop every N-th packet in the transport; assert the
  receiver's output cadence stays constant and the concealed-frame counter
  matches the drop count.
* Tick-rate independence — drive the engine at 30 Hz and at 200 Hz; both
  must produce continuous audio (30 Hz with the documented degradation).
* A/V sync — two engines, one sending audio *and* video, with a
  synthetic skew between their capture timelines; the receiving engine's
  audio playout delay converges to the correction and
  `Stats::av_sync_frames_aligned` counts the aligned frames (C-4).

### V-3. Interop (Chrome)

* Extend `interop/chrome/test_chrome_opus.html` so that the **Chrome →
  NimRTC** direction is observable: the page reports its *outbound* audio
  statistics and NimRTC reports `audio_frames_received > 0`.
* The existing NimRTC → Chrome assertions stay green (no regression in the
  direction that already works).
* The SDP fixture `interop/fixtures/chrome_opus_offer.sdp` is updated for
  the new `a=ptime` / `a=fmtp` / `a=extmap` lines, and
  `chrome_opus_offer.expected.md` is regenerated.

### V-4. Acceptance criteria for the audio pipeline

1. Chrome → NimRTC audio is decoded and delivered to the host callback as
   PCM (not as an Opus bit-stream masquerading as floats).
2. A dropped packet produces concealment audio, the output cadence stays
   constant, and the concealed frames are flagged as such.
3. The SDP a NimRTC offer produces is accepted by Chrome with `ptime`,
   `fmtp` and the audio-level extension.
4. Chrome reports `inbound-rtp` loss/jitter for NimRTC's audio (an RR is
   sent).
5. Two engines in one process have independent SSRC / sequence / timestamp
   state.
6. No silent audio loss: an unsupported PCM rate produces a clear error
   instead of silence.
7. The host's chosen API surface is complete and stable: the new
   callback, the negotiated-format accessor, the config fields and the
   statistics fields all exist, all carry a stability marker, and the
   API-freeze scan reports 0 unclassified symbols (E-8).
8. A host that only uses the legacy callback still compiles and still
   runs, unchanged (E-9).
9. The audio path is diagnosable from the log alone: an induced decode
   failure appears as exactly one throttled `Warn` line with the
   `audio-rx:` prefix, and the same fact is visible in `Stats` (L-9,
   L-11).
10. A peer that offers PCMU is answered with `PCMU/8000`; the engine
    accepts an 8000 Hz PCM rate for it and rejects 48000 Hz with
    `kAudioClockMismatch` (C-5).
11. With both tracks active the engine aligns the audio playout to the
    video capture timeline; with `av_sync_enabled == false` it does not
    (C-4).

### V-5. Static-analysis and CI gates

These are the mechanical gates the change must pass. They are listed as
verification items because a gate that is not named is a gate that gets
skipped.

| Gate | Command | What it proves |
|---|---|---|
| API freeze | `node scripts/api_freeze_scan.js` | every new public symbol carries a marker; `Missing == 0` (E-8) |
| Marker include hygiene | `python scripts/check_api_marker_includes.py` | every new public header includes `<nimrtc/core/api_version.hpp>` |
| Release freeze | `bash scripts/check-release-freeze.sh` | the header-level marker rule holds for all public headers |
| Root cleanliness | `bash scripts/check-root-clean.sh` | no build artefacts leaked into the project root |
| Markdown encoding | `pwsh scripts/check-markdown-utf8.ps1` | the spec document itself is valid UTF-8 (the repo enforces UTF-8 on all markdown) |

---

## 11. Part G — dependency order (not a schedule)

> This part lists **what must exist before what**. It is not a plan, not a
> milestone list, and carries no version numbers — the development plan and
> the version plan are decided separately (DEC-4).

```
A-6  per-engine send state ──────────┐
                                      ├──► B-5 RFC 7587 framing ──► B-6 accumulator
B-1  IAudioDecoder seam ──► B-10 Opus impl ──► B-2 receiver module ──► B-4 PT routing
                                      │                                    │
B-3  IJB via seam + clock rate ───────┘                                    │
                                                                           ▼
                                                    A-1 clock-driven tick ──► A-2/A-3/A-4/A-5
                                                                           │
E-1..E-9 new SDK  ◄─────────────────────────────────────────────────────────┘

B-15 registry/build wiring ──► B-16 resolution policy ──► B-11 engine wiring
                                                                   │
                              B-12 RR ◄──────────────────────────────┤
                                                                   ▼
                                     B-13 statistics ──► B-14 SDP completion
                                     B-17 capture timestamp (needs ADR-017 SR/abstime)
                                                          │
                                                          ▼
                                      C-4 A/V sync (needs B-17 + the video
                                      capture instants of the same timeline)

B-1  IAudioDecoder seam ──► C-5 G.711 module (same seam, a second
                              implementation — no new seam)
```

The critical path for "Chrome → NimRTC works" is:
**B-1 → B-10 → B-2 → A-1 → A-2/A-3 → B-11 → E-1/E-2**.
Everything else in Part A and B can proceed in parallel with it.

Two ordering facts that are easy to get wrong:

* **B-15 precedes B-11.** The engine can only resolve a decoder and a
  receiver once the registry slots and the registration macros exist.
  Wiring the engine first would compile against a registry that does not
  yet have the categories.
* **B-17 depends on the cross-end timeline seam** (ADR-017), which the
  video path already uses. It is *not* on the critical path — the
  `capture_time_us` field can ship as 0 until the mapping exists — but
  the field must never be faked in the meantime (B-17).
* **C-4 depends on B-17 and on the video capture instants.** The A/V
  synchronisation is meaningless without both sides of the timeline; it
  ships with the feature, not before it. The dependency is on the
  *timeline*, not on the video pipeline's other work — the video side's
  own render clock stays where the video plan puts it.
* **C-5 depends on B-1 only.** The G.711 module is a second
  implementation of the decoder seam that already exists; it introduces
  no new interface.
* **Part L has no ordering constraint of its own**, but every part above
  owes it its mandatory log points (L-2).

---

## 12. Open items for the owner

These are the points where this specification had to make a call that the
owner may want to revisit. Each has a default so that implementation is
never blocked.

| # | Question | Default taken here |
|---|---|---|
| O-1 | Should the receive pipeline live behind a new plugin seam (`plugins::IAudioReceiver`, DEC-5) or inside the engine? | New seam + `src/modules/audio_rx/` module. Rationale: L4 (engine orchestrates plugins only), unit-testability, and it stops the engine from growing another concrete include. Cost: one more registry category. |
| O-2 | The legacy callback's `size_t` argument — total interleaved floats, or samples per channel? | Total interleaved floats, because that is what the current code already passes; changing it would be a silent behavioural break for existing hosts. Documented explicitly (E-2). |
| O-3 | FEC recovery costs one frame of latency on the recovered stream. Acceptable? | Yes, accepted, flagged per frame via `fec_recovered`. The alternative (no lookahead) makes in-band FEC useless. |
| O-4 | Audio tick rate raised from ≥ 30 Hz to ≥ 100 Hz. Acceptable for existing hosts? | Yes — it is a documentation change; a slower tick degrades granularity but does not break the pipeline. |
| O-5 | Should `pcm_sample_rate_hz != 48000` be a hard error (A-7) until a resampler exists? | Yes, hard error with `kAudioClockMismatch`. Silently resampling would contradict DEC-3. **Generalised by C-5 (O-13)**: the hard error is now "the PCM rate does not match the *negotiated codec's* clock rate" — 48000 for Opus, 8000 for G.711 — so the rule survives the arrival of a second codec. |
| O-6 | Is the `0x1A40..0x1A48` error-code range acceptable? | Yes — it extends the existing audio/video block without touching a published value. |
| O-7 | Should a missing decoder or receiver plugin be a hard error, or should the engine fall back to a minimal built-in receiver? | Hard error, no fallback (B-16). A fallback receiver that decoded nothing would reproduce F-2 under a new name. The cost is that a consumer who deliberately excludes the Opus module now sees an explicit error instead of silence. |
| O-8 | Should the receiver downmix to `pcm_channels` by default, or deliver the peer's native channel count and let the host mix? | Downmix by default, with `EngineConfig::audio_downmix_to_pcm_channels` to turn it off (E-3). NimRTC's own capture side is mono, so a mono-only host should not have to write the downmix itself. The reported `channels` is always the post-downmix count, so the host never has to guess. |
| O-9 | Should `capture_time_us` be filled from the ADR-017 cross-end timeline on the receive path, or left at 0 for now? | Fill it from the timeline (B-17); 0 means "not yet reconstructible" and is never faked from the local playout clock. This is what makes the C-4 A/V-sync contract possible without a second mechanism. |
| O-10 | Should the audio frame duration be configurable per engine, or fixed at 20 ms? | Configurable via `EngineConfig::audio_ptime_ms` (10/20/40/60), defaulting to 20 (E-3, B-5, B-6). The Opus frame sizes are fixed anyway; only the packetisation granularity varies. |
| O-11 | Should `Engine::Stats` be promoted to STABLE while it is being extended? | No. It stays `NIMRTC_API_UNSTABLE` (E-4). Promotion is an unrelated policy decision. |

### The four items of 2026-11-10

| # | Question | Default taken here |
|---|---|---|
| O-12 | Should the A/V synchronisation live in the engine or in the host? | **In the engine, switchable** — `EngineConfig::av_sync_enabled` (default `true`). The owner decided this on 2026-11-10 ("the engine does it; whether synchronisation is on can be a configuration"). The engine only aligns the *audio playout*; it never resamples and never re-times video, and the host keeps the last word through the switch (C-4). |
| O-13 | Should G.711 (PCMU / PCMA) ship in `v1.2.0` or stay on the P2 line? | **In `v1.2.0`** — owner decision of 2026-11-10. It closes F-26 and provides the two mandatory WebRTC codecs. The cost: `pcm_sample_rate_hz` must equal the *negotiated codec's* clock rate (8000 for G.711), which generalises the Opus-only rule that O-5 stated; and the module adds one public header (E-8, amendment A-1) (C-5). |
| O-14 | Should the log surface be a binding contract (mandatory points, fixed levels) or left to the author's taste? | **A binding contract** (Part L). The audio path's failures are silent by nature, and a log that depends on the author's mood is not a diagnostic. The cost: every new log point needs a level and a prefix, and the review checks it. The counter-argument — "a contract on logs is a contract on prose" — is answered by keeping the mandatory list short (twelve points). |
| O-15 | Should `av_sync_enabled` default to on or off? | **On** (default `true`), but inert in an audio-only session, so an audio-only host is unaffected. A host with both tracks that wants the pre-`v1.2.0` behaviour turns one field off. The alternative — default off — would ship a feature nobody benefits from while the behaviour change is invisible until it is switched on. |

---

## 13. Appendix — traceability

| Requirement / finding | Covered by |
|---|---|
| F-1, F-2, F-4 — no decoding on the receive path | A-2, B-1, B-2, B-10, B-11 |
| F-3 — garbage AEC reference | A-10, B-2 (3A stays in the engine) |
| F-5 — arrival-driven playout | A-1, A-4, B-2 |
| F-6, F-7 — wrong clock rate, concrete JB in the engine | B-3 |
| F-8, F-9, F-10, F-11 — no negotiation, no FEC, no PLC | A-3, A-9, B-4, B-7, B-8, B-10 |
| F-12, F-13 — shared static send state | A-6, B-14 |
| F-14 — RFC 7587 unused | B-5 |
| F-15, F-16 — timestamp correctness | A-7, B-5 |
| F-17, F-19 — silent loss, hard-coded encoder config | A-8, C-1 |
| F-18 — channel contradiction | A-5, B-4, B-9 |
| F-20, F-21, F-22 — no RR, no stats, incomplete SDP | B-12, B-13, B-14, E-4, E-8 |
| F-23, F-24, F-25, F-26, F-27 — missing capability | C-2, C-3, C-4, C-5, V-1 … V-5 (verification) |
| F-25 — no A/V synchronisation | C-4, V-1, V-2, V-4 |
| F-26 — no PCMU / PCMA | C-5, X-2, V-1, V-4 |
| DEC-1 — new codec seam | B-1, B-10, B-15 |
| DEC-2 — legacy + new callback | E-1, E-2, E-9 |
| DEC-3 — native rate, explicit rate/channel reporting, no resampler | A-2, A-5, E-1, X-1 (out of scope) |
| DEC-4 — spec + design only | Part G is dependency order only; no schedule, no version numbers |
| DEC-5 — two new plugin seams | B-1, B-2, B-15 |
| DEC-6 — two new registry categories | B-15, B-16 |
| A-1 … A-10 — P0 receive/send correctness | V-1, V-2, V-4 |
| B-1 … B-17 — P1 interfaces and wiring | V-1, V-2, V-3, V-5 |
| C-1 … C-5 — quality and platform | C-4 and C-5 are part of the v1.2.0 body (O-12, O-13); C-1 … C-3 stay on the P2 line and are scheduled later (DEC-4) |
| E-1 … E-9 — the public SDK | V-1, V-4, V-5 |
| Amendment A-1 — the public symbols added by C-4 / C-5 | E-8, V-5 |
| L-1 … L-12 — the logging contract | Part L, V-1, V-4 |
| O-1 … O-15 — owner open items | each has a default; none blocks implementation |
