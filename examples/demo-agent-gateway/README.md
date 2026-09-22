# demo-agent-gateway

> AI Agent ingress demo: NimRTC engine + Profile library + WebRTC
> APM audio 3A + mock ASR + mock LLM. Validates the
> `agent-gateway` profile (L0+L1+L2 with PCM tap enabled) on the
> way to the AI-Agent use case.

---

## 1. What this demo proves

1. **Profile loading** through the Builder:
   `Builder{}.load_profile("agent-gateway").build()` produces a
   `Profile` struct that drives every engine plug-in (DTLS, audio3a,
   codec, JB).

2. **PCM tap installation** on `IAudio3A`:
   `set_pre_process_tap(...)` and `set_post_process_tap(...)` (with
   both `float*` and `int16_t*` overloads — see RFC 001) — taps
   fire correctly with mock PCM.

3. **Mock ASR** — energy-based VAD produces a stub transcript
   whenever RMS > -40 dBFS.

4. **Mock LLM** — keyword match on `"hello"` produces a canned
   reply.

5. **Plugin fallback path** — the demo instantiates `IAudio3A`
   directly from `core::PluginRegistry` (the same code path the
   engine uses internally) so the demo is resilient across build
   configurations where the vendored WebRTC APM may or may not be
   present.

---

## 2. Build & run

### 2.1 Build

```bash
cmake --preset debug -DNIMRTC_BUILD_EXAMPLES=ON
cmake --build build --target demo-agent-gateway
```

### 2.2 Run

```bash
# Default: 3 seconds of synthetic audio
./build/examples/demo-agent-gateway

# Custom duration (seconds, decimal ok)
./build/examples/demo-agent-gateway 5
./build/examples/demo-agent-gateway 0.5
```

Expected output (trimmed):

```
[demo-agent-gateway] starting with agent-gateway profile
[Profile] agent-gateway profile loaded
  - audio3a: webrtc_apm
  - codec:   opus
  - jb_delay: 30 ms
[Engine] open: 0
[Audio3A] obtained plugin 'webrtc_apm' from registry
[Demo] PCM taps installed
[Demo] starting tap test loop (3.0 seconds)
[Tap] pre  frame: ts=10000, samples=480, rate=48000, ch=1, level=-3.0 dBFS
[ASR]   speech: -3.0 dBFS -> "hello world"
[LLM]   Hi! I heard you say: 'hello world'
[Tap] pre  frame: ts=30000, samples=480, rate=48000, ch=1, level=-3.0 dBFS
[ASR]   silence: -96.0 dBFS
...
[Demo] processed 300 frames in 3.0 seconds
[demo-agent-gateway] done
```

Exit code: always 0.

### 2.3 Cross-platform

- **Windows**: same flags; built with MSVC default.
- **Linux / macOS**: same flags; ninja generator recommended.
- **aarch64**: compiles; runtime is x86_64-debug-equivalent (same
  behaviour because the demo doesn't depend on platform-specific
  audio HAL).

---

## 3. What you can change

### 3.1 Switch audio3a backend

The default is `webrtc_apm`. To use a stub (cheaper, no 3A):

```cpp
// In run_demo(), substitute:
cfg.audio3a_name = "";   // engine picks Null fallback
// and likewise for obtain_audio3a() below
```

Or supply your own:

```cpp
obtain_audio3a("webrtc");      // built-in null stub
obtain_audio3a("my3aplugin");  // if you've implemented one
```

### 3.2 Switch profile

This demo loads `agent-gateway` but every other profile works the
same way:

```cpp
Profile profile = Builder{}.load_profile("call").build();
Profile profile = Builder{}.load_profile("teleop").build();
Profile profile = Builder{}.load_profile("agent-low-latency").build();
```

See `docs/profiles.md` for the full matrix.

### 3.3 Real ASR

Replace `mock_asr()` with your ASR client. The tap callbacks pass:

- pre-3A: `(const float* pcm, const PcmFrameMetadata& m)`
- post-3A: `(const float* pcm, const PcmFrameMetadata& m)` OR
  `(const int16_t* pcm, const PcmFrameMetadata& m)` — depending on
  which tap you install.

`PcmFrameMetadata` carries `timestamp_us`, `sample_rate_hz`,
`num_samples`, `num_channels`. For ASR intents you usually want
`post-3a` int16 (matches Whisper / vosk / Picovoice buffer
shape).

### 3.4 Real LLM

Replace the `std::string text = "hello world"` and the keyword
match with your client call. The tap fires on **every PCM frame**
(typically 10–20 ms each); batch frames to 30 ms or 100 ms before
calling the LLM to avoid runaway token costs.

---

## 4. Architecture map

```
engine.cfg ──► profile ──► audio3a plugin (webrtc_apm)
                              ▲
                              │
                              ├── pre  tap ──► VAD / wake-word
                              │
                              ├── post tap (float)  ──► custom analyzer
                              │
                              └── post tap (int16)  ──► ASR (Whisper / vosk)
                                                          │
                                                          ▼
                                                       LLM client
                                                          │
                                                          ▼
                                                   TTS / Agent reply
```

Profiles dictate which modules are linked. The `agent-gateway`
profile enables everything in L0–L2, lets the engine drive Opus +
DTLS + SRTP, but **doesn't** perform TTS — that is the calling
Agent's job. Run two such engines against each other to chat.

---

## 5. Common pitfalls

- **"Plugin not found" at open** — `core::register_all_default_plugins()`
  was not called. The demo calls it inside `engine.open()` (because
  `init_modules_once` does it); you don't need a separate call —
  unless you intend to bypass `engine.open()`, in which case call
  it explicitly.
- **Tap callback fires but PCM is silence** — your `process_capture`
  cadence is faster than the source rate. The mock here uses
  10 ms / 480-sample frames at 48 kHz; match it.
- **Same buffer address in every frame** — implementations may
  reuse a single PCM ring buffer. Treat the buffer as read-only and
  copy out before the next tick.

---

## 6. See also

- `examples/loopback-p2p/` — minimum loopback handshake smoke test.
- `examples/demo-p2p/` — minimum two-engine P2P session.
- `docs/profiles.md` — Profile matrix and authoring guide.
- `docs/rfcs/rfc-001-pcm-tap.md` — RFC for the tap surface.
- `docs/plugin_author_guide.md` — how to write a custom audio3a
  plugin (e.g. wake-word first-pass VAD).
- `src/plugins/include/nimrtc/plugins/audio3a.hpp` — interface.
- `src/modules/audio3a/` — built-in implementations.

> **One-line summary:** loads `agent-gateway` profile, drives a
> 3-second PCM stream through WebRTC APM, taps both pre/post 3A,
> runs an energy-VAD ASR + keyword LLM. Use as the starting point
> for any Agent integration.
