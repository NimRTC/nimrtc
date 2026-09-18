# RFC 001: PCM Tap for AI Agent Integration

| | |
|---|---|
| RFC | 001 |
| Title | PCM Tap — Pre/Post-3A Audio Frame Hooks for AI Agent Pipelines |
| Author | BDFL |
| Status | **Draft** |
| Date | 2026-09-18 |
| Supersedes | — |
| Target | v0.11.0 (P2) — TAP-1 per `docs/plan/v0.11-plan.md` §2.1.1 |
| References | `docs/zh/architecture.md` §8.7; `src/plugins/include/nimrtc/plugins/audio3a.hpp`; ADR-009; ADR-010 |

---

## Summary

This RFC documents the PCM Tap interface — two synchronous callback hooks on the
`IAudio3A` capture path — and the intended usage patterns for AI Agent pipelines.
The interface is already present in `audio3a.hpp` as `set_pre_process_tap()` and
`set_post_process_tap()`. This RFC provides the rationale, usage contract, and
profile-integration story so that the v0.11.0 agent-gateway demo (DEMO-1) has
a clear specification to implement against.

---

## 1. Motivation

AI Agent pipelines require access to raw or processed audio frames at sub-second
latency. A speech-to-text (ASR) pipeline needs clean PCM input; a wake-word
detector needs the unmodified microphone signal. In NimRTC v0.9.0-rc1 the tap
interface was introduced as a P1 placeholder — this RFC formalises the contract
for P2, enabling the first agent-gateway demo to ship in v0.11.0.

The core design goals are:

1. **Zero-copy where possible** — taps receive `const` pointers; the engine
   does not allocate or copy frame data for the tap.
2. **Synchronous, fast-return** — taps run inside `process_capture()` on the
   audio thread; blocking or allocating in a tap will cause audio glitches.
3. **Both raw and processed views** — `pre` tap sees the microphone signal
   before AEC/ANS/AGC; `post` tap sees the cleaned signal after all processing.
4. **ASR-friendly encoding** — `post` tap is available as both `float*` and
   `int16_t*`; most ASR engines (Whisper, Vosk, proprietary) expect int16_t
   16 kHz or 48 kHz PCM.

---

## 2. Interface Specification

### 2.1 Metadata struct

```cpp
// src/plugins/include/nimrtc/plugins/audio3a.hpp
struct PcmFrameMetadata {
    uint32_t sample_rate_hz = 48000;
    size_t   num_samples    = 0;   // samples per channel
    uint8_t  num_channels   = 1;
    int64_t  timestamp_us   = 0;   // monotonic microseconds
};
```

- `timestamp_us` is the engine's **monotonic** clock at capture time.
  It is comparable across calls and across receive-side `capture_ts` values
  for timeline correlation (§8.4 of architecture.md).
- `num_samples * num_channels * sizeof(T)` bytes are accessible at the
  pointer passed to the tap callback.

### 2.2 Tap callbacks

```cpp
// Float tap — receives native float PCM
using PcmTapCallback =
    std::function<void(const float* samples, const PcmFrameMetadata& meta)>;

// Int16 tap — receives float→int16_t conversion; ASR-friendly
using PcmTapCallbackI16 =
    std::function<void(const int16_t* samples, const PcmFrameMetadata& meta)>;
```

### 2.3 `IAudio3A` methods

```cpp
// Install / uninstall pre-3A tap
virtual void set_pre_process_tap(PcmTapCallback tap) noexcept = 0;
//   Pass nullptr to uninstall.

// Install / uninstall post-3A tap (both variants simultaneously)
virtual void set_post_process_tap(PcmTapCallback    tap,
                                  PcmTapCallbackI16 tap_i16) noexcept = 0;
//   Pass nullptr for either variant to leave it uninstalled.
```

**Thread safety**: Taps may be set/unset from any thread **between**
`process_capture()` calls. They must not be set/unset from inside a tap
callback or inside `process_capture()`.

### 2.4 Call order inside `process_capture()`

```
process_capture(float* samples, num_samples, num_channels)
  1. [pre tap]    tap_(samples, meta)          ← unmodified mic PCM
  2. AEC / ANS / AGC processing (in-place)
  3. [post tap]   tap_(samples, meta)           ← float, cleaned
  4. [post tap]   tap_i16_(samples, meta)       ← int16_t, cleaned
  5. return kOk
```

Steps 1 and 2/3/4 are synchronous and on the same audio thread.
If a tap blocks (e.g. queues to a background thread), the audio thread
is blocked. Implementors must treat this as a hard constraint.

---

## 3. Usage Patterns

### 3.1 Wake-word detection (pre tap)

```cpp
// Agent terminal: install pre tap for on-device wake-word detection
auto* audio3a = /* resolve via PAL */;
audio3a->set_pre_process_tap([](const float* samples, const PcmFrameMetadata& meta) {
    // samples is const — no copy needed for keyword spotter
    my_wake_word_model.process(meta.sample_rate_hz, meta.num_channels,
                               samples, meta.num_samples, meta.timestamp_us);
});
```

### 3.2 ASR streaming (post tap)

```cpp
// Agent gateway: install post tap for streaming ASR
auto* audio3a = /* resolve via PAL */;
audio3a->set_post_process_tap(
    /* float tap: */ nullptr,
    /* int16 tap: */ [](const int16_t* samples, const PcmFrameMetadata& meta) {
        // Convert int16_t → float for Whisper (if needed), or feed int16_t directly.
        // For streaming, push into a lock-free ring buffer consumed by
        // a background ASR thread.
        asr_ring_buffer.push(samples,
                            meta.num_samples * meta.num_channels * sizeof(int16_t),
                            meta.timestamp_us);
    });
```

### 3.3 Both taps simultaneously

```cpp
// Full pipeline: wake-word on raw, ASR on cleaned
audio3a->set_pre_process_tap([](const float* samples, const PcmFrameMetadata& meta) {
    wake_word_model.process(meta.sample_rate_hz, samples, meta.num_samples);
});
audio3a->set_post_process_tap(
    /* float: */ [](const float* samples, const PcmFrameMetadata& meta) {
        // e.g. neural codec encoder
        codec_encoder->feed(samples, meta.num_samples, meta.timestamp_us);
    },
    /* int16: */ [](const int16_t* samples, const PcmFrameMetadata& meta) {
        // e.g. Whisper streaming endpoint
        whisper->send_frame(samples, meta.num_samples);
    });
```

---

## 4. Profile Integration

Per `docs/zh/architecture.md` §2.6, the `agent` and `agent-gateway` Profiles
enable PCM tap as part of their standard composition:

| Profile | PCM tap usage | Notes |
|---|---|---|
| `agent` | Both pre + post taps available | SDK 形态，3A enabled; tap for wake-word + ASR |
| `agent-gateway` | Post tap preferred (3A bypass / passthrough) | 服务端形态; gateway side receives cleaned PCM or decoded frames |
| `sfu-agent` | Post tap via RTP decode bypass | SFU 形态; SFU decodes once and fans out to N agents |

The Profile JSON schema (ADR-010, schema v1.0) does not need a new section
for PCM tap — tap registration is an API-level action, not a declarative
configuration item. Profiles that support tap expose it in their runtime API contract.

---

## 5. Non-Goals

- **Model framework binding** — This RFC provides PCM tap only. It does not
  include OpenAI Realtime API / 豆包 specific bindings. Upper layers connect
  their ASR / LLM SDK to the tap.
- **Frame buffering / batching** — The engine does not provide a built-in
  ring buffer. Taps that need async processing must provide their own
  lock-free queue.
- **Playback-side tap** — `process_render()` does not expose tap hooks.
  Render tapping is out of scope for v0.11.0.
- **Variable sample rate** — `PcmFrameMetadata::sample_rate_hz` is always the
  configured rate (default 48000 Hz). Resampling tap output is the caller's
  responsibility if a lower rate is required.

---

## 6. Implementation Notes (for TAP-1)

The default implementation in v0.11.0 is **WebRTC APM** (vendored, per
`docs/zh/architecture.md` §11.4 WebRTC APM vendor rule).

Key decisions for the implementor:

1. **`pre` tap placement** — invoke immediately on entry to
   `WebRtcAudioProcessing::ProcessCaptureStream()`, before AEC filter applies.
2. **`post` tap placement** — invoke after `WebRtcAudioProcessing::ProcessCaptureStream()`
   returns, before returning from `process_capture()`.
3. **float→int16 conversion** — use `std::round(s * 32767.0f)` saturating to
   `INT16_MAX` / `INT16_MIN`. Clipping must be documented; ASR engines handle
   brief clipping gracefully.
4. **Timestamp source** — `timestamp_us` comes from the engine's monotonic clock
   (`std::chrono::steady_clock::now().time_since_epoch()`) captured at the
   time `process_capture()` is invoked. The value is consistent with
   `timeline` module's `capture_ts` and is usable for command-frame correlation.
5. **Null tap uninstall** — `set_*_tap(nullptr)` must be safe to call even if
   no tap is installed. The implementation checks `if (tap_)` before invoking.

---

## 7. Open Questions

| # | Question | Recommendation |
|---|---|---|
| 1 | Should `pre` tap also expose int16_t variant? | Not in v0.11.0. Wake-word models typically accept float. Add if a concrete ASR consumer requires pre-tap int16. |
| 2 | Should taps carry a sequence number or frame counter? | Add `uint32_t frame_index` to `PcmFrameMetadata` in a later patch if gap detection in ASR becomes a real problem. |
| 3 | Maximum number of simultaneous taps per stage? | v0.11.0: exactly one tap per variant (float pre / float post / int16 post). Multiple tap fan-out is the caller's responsibility (e.g. `MultiTap` wrapper). |
| 4 | Alignment / SIMD constraints on `samples` pointer? | WebRTC APM outputs 32-byte-aligned float buffers. This is sufficient for SSE/NEON. No additional guarantee is made for external plugin implementations. |

---

## 8. Change Log

| Version | Date | Change |
|---|---|---|
| 0.1 | 2026-09-18 | Initial draft |

---

## 9. Appendix: Relationship to Other Documents

- `docs/zh/architecture.md` §8.7 — Original motivation for this feature.
- `src/plugins/include/nimrtc/plugins/audio3a.hpp` — Canonical interface definition.
- `docs/plan/v0.11-plan.md` — TAP-1 is item DC-1's dependency; DEMO-1 consumes it.
- `docs/adr/ADR-009-pal-slice-1.md` — PAL Slice 1 provides the `pal::resolve_audio3a()` path by which callers obtain the 3A instance.
- `docs/adr/ADR-010-profile-json-format.md` — Profile schema v1.0; PCM tap is API-level, not a JSON config item.
