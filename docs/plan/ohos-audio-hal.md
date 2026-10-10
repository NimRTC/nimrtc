# OpenHarmony Audio HAL — Selection Decision & Integration Plan

> **Status: DECIDED** — Stage 4 of `docs/plan/openharmony-support.md`.
> Owner: BDFL. This document is the Stage 4 output; implementation is
> a v0.12.x patch follow-up, not v0.12.0 itself.

---

## 1. Decision

**Select: OHAudio** (OHOS Native API, OHOS 9+).

**Reject: OpenSL ES** (deprecated; only a fallback for OHOS SDK < 9).

OpenSL ES support is removed from OHOS 9+ and the OHOS compatibility
layer is not ABI-stable across SDK versions. OpenSL ES was already
deprecated by the Khronos Group before OHOS adopted it as a
compatibility shim. **Do not invest in OpenSL ES.**

AAudio is not available on OHOS (it is an Android-specific API). NimRTC
is C++ and does not use Java/Kotlin AudioTrack directly.

---

## 2. Rationale

| API | OHOS support | NDK-style C | Recommended | Notes |
|---|---|---|---|---|
| **OHAudio** (Native API) | OHOS 9+ ✅ | ✅ C API (`OH_AudioStreamBuilder`, `OH_AudioRenderer`, `OH_AudioCapturer`) | **✅ 推荐** | First-class NDK API; lifecycle mirrors AAudio but with OHOS-specific audio focus; stable across OHOS 9/10/11 |
| OpenSL ES | OHOS compatibility layer ✅ | ✅ C API | ❌ Deprecated | Only for SDK < 9 fallback; not available in OHOS 9+; no security updates |
| AAudio | ❌ | ✅ C API | ❌ | Android-only; OHOS does not ship AAudio |
| AudioTrack | ✅ | ❌ Java/Kotlin | ❌ | Not usable from NimRTC C++; requires JNI bridge |

OHAudio's API surface is well-documented in the OHOS Developer
Documentation (`developer.huawei.com`). The C API signature closely
mirrors Android's AAudio, which means NimRTC's existing `IAudioSource`
/ `IAudioSink` PAL seam (designed for hardware codec backends) maps
directly.

---

## 3. Integration plan

### 3.1 Target interfaces

NimRTC's audio plugin seam is defined by two PAL interfaces:

```cpp
// src/plugins/include/nimrtc/plugins/audio_source.hpp
namespace nimrtc::plugins {
struct IAudioSource {
    virtual ~IAudioSource() = default;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void set_on_audio_frame(OnAudioFrameCb cb) = 0;
};

// src/plugins/include/nimrtc/plugins/audio_sink.hpp
struct IAudioSink {
    virtual ~IAudioSink() = default;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void set_on_audio_ready(OnAudioReadyCb cb) = 0;
};
}
```

The existing `audio3a` module implements both: `AudioSource` wraps
`IVoiceEngine` (WebRTC APM) in the non-OHOS build. On OHOS, we
add `OHAudioSource : public IAudioSource` and
`OHAudioSink : public IAudioSink` in a new file
`src/plugins/ohos/ohos_audio_hal.cpp`.

### 3.2 OHAudio C API mapping

OHAudio NDK header: `<audio_native.h>` (part of OHOS Native API).

**Audio capture path** (`IAudioSource` ← `OH_AudioCapturer`):

```cpp
// Pseudo-code — actual C++ wraps the OH_AudioCapturer C API
class OHAudioSource : public plugins::IAudioSource {
public:
    void start() override {
        OH_AudioStreamBuilder* builder = nullptr;
        OH_AudioStreamBuilder_Create(&builder, AUDIO_STREAM_TYPE_CAPTURER);

        // 48 kHz mono — match NimRTCEngineConfig::pcm_sample_rate_hz / pcm_channels
        OH_AudioStreamBuilder_SetSamplingRate(builder, 48000);
        OH_AudioStreamBuilder_SetChannelCount(builder, 1);
        OH_AudioStreamBuilder_SetSampleFormat(builder, SAMPLE_S16LE);
        OH_AudioStreamBuilder_SetRendererCallback(builder, capturer_callback_, this);

        OH_AudioStreamBuilder_GenerateSessionId(builder, &session_id_);
        OH_AudioStreamBuilder_SetVolume(builder, 1.0f);
        OH_AudioStreamBuilder_Build(builder, &capturer_);  // captures to NimRTC
    }

    void set_on_audio_frame(OnAudioFrameCb cb) override {
        capturer_callback_ = [this, cb](const void* buffer, size_t num_frames) {
            // buffer is int16_t PCM; wrap in AudioFrame and forward
            AudioFrame frame{buffer, num_frames, 48000, 1, SAMPLE_S16LE};
            cb(frame);
        };
    }

private:
    OH_AudioCapturer* capturer_ = nullptr;
    int32_t session_id_ = 0;
    OnAudioFrameCb capturer_callback_;
};
```

**Audio render path** (`IAudioSink` ← `OH_AudioRenderer`):

```cpp
class OHAudioSink : public plugins::IAudioSink {
public:
    void start() override {
        // Mirror of capture: create OH_AudioRenderer, set render callback
        // render callback reads from NimRTC's send-audio path
    }

    void set_on_audio_ready(OnAudioReadyCb cb) override {
        // cb supplies PCM frames from NimRTC engine's audio pipeline
        // renderer callback drains the queue
    }
};
```

### 3.3 Registration

Register both via the existing plugin registry:

```cpp
// src/plugins/ohos/ohos_audio_plugin.cpp

// Same pattern as nimrtc_link_gmssl() but for OHAudio:
option(NIMRTC_ENABLE_OHAUDIO "Build the OHAudio audio HAL adapter (OpenHarmony)" OFF)

#if NIMRTC_ENABLE_OHAUDIO
    // Register OHAudioSource / OHAudioSink factories
    core::Registrar r;
    r.register_audio_source("ohos", &OHAudioSource::factory);
    r.register_audio_sink("ohos", &OHAudioSink::factory);
#endif
```

The `audio3a` PAL slot already accepts any `IAudioSource` /
`IAudioSink` implementation. No engine change is required — only a
CMake option + the two adapter classes.

### 3.4 Timeline and ordering

| Step | Description | Prerequisite |
|---|---|---|
| 1 | Fetch OHAudio NDK headers from OHOS SDK | OHOS SDK installed |
| 2 | Implement `OHAudioSource` adapter class | `IAudioSource` interface |
| 3 | Implement `OHAudioSink` adapter class | `IAudioSink` interface |
| 4 | Add `NIMRTC_ENABLE_OHAUDIO` CMake option + plugin registration | — |
| 5 | Build `OHAudioSource` + `OHAudioSink` in OHOS preset | Steps 1–4 |
| 6 | Manual test: `demo-agent-gateway` with real microphone on OHOS device | Steps 1–5 |
| 7 | PR + review | — |
| 8 | `docs/plan/openharmony-support.md` OH-DOD-4 / OH-DOD-5 close | Steps 1–7 |

### 3.5 Risk

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| OHOS SDK NDK headers differ across OHOS 9/10/11 | Medium | Build fails | Use `OHOS_SDK_VERSION` CMake variable to guard API version checks |
| OHAudio session ID conflicts with OHOS media framework | Low | Audio route wrong | Request a dedicated `AUDIO_SESSION_ID_OHOS` via `OH_AudioStreamBuilder_SetVolume` before start |
| 48 kHz / 16-bit PCM semantics differ across devices | Medium | Audio quality | NimRTC's `IAudioSource` accepts any sample rate; resample in `OHAudioSource` if device caps at 44.1 kHz |
| OHOS emulator audio is unreliable (headless) | High | Cannot run unit tests on CI | Gate test on device; CI tests the build only |

---

## 4. References

- OHOS Developer Documentation: [Audio Capturer (Native API)](https://developer.huawei.com/consumer/cn/doc/h开发指南-V5开发音频-0000001892653941-V5)
- OHOS Developer Documentation: [Audio Renderer (Native API)](https://developer.huawei.com/consumer/cn/doc/h开发指南-V5开发音频-0000001892653941-V5)
- `docs/plan/openharmony-support.md` §4.3 (R3 — OHAudio / OpenSL ES 适配) — the audit that produced this document
- `src/plugins/include/nimrtc/plugins/audio_source.hpp` — `IAudioSource` interface
- `src/plugins/include/nimrtc/plugins/audio_sink.hpp` — `IAudioSink` interface
- `src/modules/audio3a/src/audio3a.cpp` — existing `AudioSource` / `AudioSink` reference implementation (WebRTC APM path)
