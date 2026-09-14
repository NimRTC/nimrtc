# ADR-008: PCM Tap Interface for AI Agent 3A Bypass

**Status:** Accepted
**Date:** 2026-09-04 (proposed), 2026-09-07 (accepted; interfaces merged)
**Phase:** P1.5 (interfaces merged ahead of P2 schedule for AI Agent scenarios)

---

## Context

AI Agent scenarios (voice assistants, meeting bots, on-premise transcription) require access to raw or processed audio for:

1. **Wake-word detection** — must see the raw microphone signal before any audio processing (AEC/ANS/AGC) to avoid false triggers from echo or noise suppression.
2. **Automatic Speech Recognition (ASR)** — prefers 3A-cleaned PCM (after echo cancellation, noise suppression, and gain control) to minimize WER (Word Error Rate).
3. **Audio monitoring / logging** — may need either raw or processed PCM for quality assurance.

The current `IAudio3A` interface processes audio but does not expose taps to inspect the signal at different pipeline stages.

---

## Decision

Add two synchronous tap callbacks to `IAudio3A`:

```cpp
// plugins/audio3a.hpp

struct PcmFrameMetadata {
    uint32_t sample_rate_hz = 48000;
    size_t   num_samples    = 0;   // per-channel
    uint8_t  num_channels   = 1;
    int64_t  timestamp_us   = 0;   // monotonic microseconds
};

using PcmTapCallback   = std::function<void(const float*, const PcmFrameMetadata&)>;
using PcmTapCallbackI16 = std::function<void(const int16_t*, const PcmFrameMetadata&)>;

class IAudio3A : public IPlugin {
public:
    // ... existing methods ...

    virtual void set_pre_process_tap(PcmTapCallback tap) noexcept = 0;
    virtual void set_post_process_tap(PcmTapCallback    tap,
                                     PcmTapCallbackI16 tap_i16) noexcept = 0;
};
```

### Tap semantics

| Tap | Stage | Use case | Data |
|-----|-------|----------|------|
| `pre_process_tap` | Before AEC/ANS/AGC | Wake-word, raw recording | `float*`, float32 [-1.0, 1.0] |
| `post_process_tap` | After AEC/ANS/AGC, before encode | ASR, monitoring | `float*` and/or `int16_t*` |

### Invocation order in `process_capture()`

```
1. [pre_process_tap] ← raw mic PCM (for wake-word / monitoring)
2. AEC / ANS / AGC
3. [post_process_tap] ← 3A-cleaned PCM (for ASR)
4. encode
```

### Thread safety

- Taps are **synchronous** — invoked inline inside `process_capture()`, which is called from a single audio thread.
- `set_pre_process_tap()` / `set_post_process_tap()` may be called from any thread **between** `process_capture()` calls (not inside a call).
- Implementations store the callback atomically (`std::function` move/copy is safe).

### Memory model

- **No extra copy if not needed**: the float tap receives a pointer into the existing buffer; the caller retains ownership.
- **int16_t conversion happens only if `post_tap_i16` is set**: float→int16 is done inline with a small stack buffer.

---

## Alternatives Considered

### 1. Separate interfaces (IAudioTapPre, IAudioTapPost)

Rejected: adds two new interface files and factories for a trivial feature. Single interface with optional callbacks is cleaner.

### 2. Non-owning buffer copy in tap callback

Rejected: would require the engine to allocate a copy for every frame when a tap is installed, even if the tap doesn't need it. Synchronous inline invocation is zero-cost when no tap is installed.

### 3. Ring-buffer based tap delivery (async)

Rejected: AI agents need low-latency access (wake-word < 100 ms). Async delivery adds buffering latency and complexity. Synchronous tap is simpler and sufficient.

### 4. Template-based tap (compile-time injection)

Rejected: requires recompilation to change tap behavior. Runtime `std::function` callback is sufficient.

---

## Consequences

**Positive:**
- Wake-word engines can inspect raw mic PCM before any processing.
- ASR engines receive 3A-cleaned PCM in their preferred format (int16_t).
- Zero-cost when not used (no extra allocation or copy).
- Minimal interface change — two virtual methods added to existing `IAudio3A`.

**Negative:**
- Tap callbacks are synchronous and run in the audio thread. Long-running tap callbacks will block audio processing.
- `std::function` has small heap allocation on first capture for non-capturing lambdas (mitigated by using captureless lambdas where possible).

**Neutral:**
- Binary layout of `IAudio3A` changes (two new virtual methods). This is a breaking change for any out-of-tree implementations — document as P2.
- Thread safety is the caller's responsibility (same as existing callbacks).

---

## Implementation

- `src/plugins/include/nimrtc/plugins/audio3a.hpp` — interface definitions
- `src/modules/audio3a/src/audio3a_plugin.cpp` — `PluginAdapter` implements the tap methods with safe no-op defaults
- `tests/plugins/test_audio3a_tap.cpp` — GoogleTest proving round-trip (100 frames, tap count verification)

---

## References

- [ADR-001: Pluggable Module Architecture](ADR-001-plugin-system.md)
- [NimRTC V2 Technical Doc §8.7: Pre/post 3A dual PCM tap](TBD)
