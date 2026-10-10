# NimRTCEngine — API Reference

> Single-header reference for `nimrtc::engine::NimRTCEngine`.
>
> Header: `src/engine/include/nimrtc/engine/engine.hpp`
> Status: Public API for v0.12.0-alpha. Pre-1.0; minor breaking
> changes are noted in `CHANGELOG.md` and the per-method `@note`
> tags.

This document covers every public method, its parameters, return
values, and lifecycle expectations. For the configuration struct see
[engine_config.md](./engine_config.md).

---

## 1. Quick reference

```text
nimrtc::engine::NimRTCEngine
│
├── Lifecycle
│   ├── ctor(EngineConfig)
│   ├── ~NimRTCEngine()
│   ├── open() → Status
│   ├── pre_open() → Status
│   ├── close()
│   └── is_open() → bool
│
├── Callbacks (set once at construction)
│   ├── set_on_audio_frame(cb)
│   ├── set_on_video_frame(cb)
│   ├── set_on_error(cb)
│   ├── set_on_state_change(cb)
│   ├── set_on_data_message(cb)
│   └── set_on_data_state(cb)
│
├── SDP
│   ├── create_offer() → string
│   └── process_remote_sdp(string) → optional<string>
│
├── ICE inspection / control
│   ├── ice_state_string() → cstring
│   ├── local_ufrag() / local_password()
│   ├── add_turn_server(...)
│   └── set_remote_ice(string) → bool
│
├── DTLS / SRTP inspection
│   ├── dtls_state() → DtlsState
│   ├── dtls_connected() → bool
│   ├── srtp_installed() → bool
│   ├── local_dtls_fingerprint_sha256_base64() → string
│   ├── drain_dtls() → int
│   └── maybe_install_srtp_keys()
│
├── Audio I/O
│   └── send_audio(const float* pcm, size_t n) → Status
│
├── Video I/O
│   ├── start_video() / stop_video()
│   └── send_video(const VideoSourceFrame&) → Status
│
├── Data / control plane
│   └── create_data_channel(label) → unique_ptr<IDataChannel>
│
├── Tick loop
│   └── tick() → int
│
├── Plugin accessors (read-only after open())
│   ├── transport() / ice_transport()
│   ├── audio3a() / codec_plugin()
│   ├── video_source() / sink() / receiver() / sender()
│   ├── bwe() / scheduler() / video_codec()
│   └── config() → const EngineConfig&
│
└── Runtime swaps
    ├── set_video_sink(unique_ptr<IVideoSink>)
    ├── set_video_source(unique_ptr<IVideoSource>)
    └── Stats::srtp_drops / stats()
```

---

## 2. Construction & ownership

```cpp
#include <nimrtc/engine/engine.hpp>

nimrtc::engine::EngineConfig cfg;
cfg.local_bind_address = "127.0.0.1";
// ...

nimrtc::engine::NimRTCEngine engine(cfg);
```

- The engine is **move-friendly at the C++ level** but **`open()`
  must be called exactly once** on the live instance. After
  `close()`, the engine is reusable for a fresh SDP exchange but the
  ICE/DTLS state must be torn down on the network side.
- Copy and assignment are deleted.

---

## 3. Lifecycle

### 3.1 `open()`

```cpp
uint32_t open() noexcept;
```

Wires every plugin declared in `config_`, starts ICE candidate
gathering, and prepares the engine for SDP exchange. Returns
`kOk` (0) on success, otherwise a non-zero error code that is
also fired via `on_error_`. Inspect with `last_open_rc()`.

Internally:

1. calls `core::register_all_default_plugins()` so the built-in
   plugins are visible to the registry;
2. resolves all 13 typed factories (transport, RTP, SDP, JB, audio3A,
   codec, BWE, scheduler, DTLS, SCTP, video codecs, …);
3. opens the ICE transport, audio3A, codec, BWE, scheduler;
4. wires the DTLS session, SRTP context, audio encode/decode chain.

Failure modes (non-exhaustive, all returned as non-zero `uint32_t`):
`kErrResourceExhausted`, `kErrNotFound` (factory id not registered),
`kErrInternal`. See `last_open_rc()` and `on_error_` for diagnostics.

### 3.2 `pre_open()`

```cpp
uint32_t pre_open() noexcept;
```

Equivalent to `open()` **minus** the final `ice_t_->open()` call
(so ICE candidate gathering does not start). Use this when:

- You want to call `set_remote_ice(peer_block)` *before* ICE
  gathering — required for the loopback test to avoid ICE role
  conflict: the answerer must know the offerer's ICE ufrag/pwd
  before its own ICE starts, so libjuice picks `controlled` (instead
  of `controlling`).

After `pre_open()` you call `open()` normally; ICE gathering fires
on `open()`. `pre_open()` returns the same error codes as `open()`.

### 3.3 `close()`

```cpp
void close() noexcept;
```

Releases all plugin instances and tears down network state. Safe to
call multiple times; subsequent calls are no-ops. Idempotent with
the destructor.

### 3.4 `is_open()`

```cpp
bool is_open() const noexcept;
```

True iff the engine is in the `kOpen` state (after a successful
`open()`, before any `close()`).

---

## 4. Callbacks

All callbacks are user-owned `std::function`s; pass `nullptr` to
clear. They are installed before `open()` and re-fired for the
lifetime of the engine. None of them may throw — `noexcept`-marked
methods are written defensively.

| Callback | Fires when | Signature |
|---|---|---|
| `on_audio_frame` | A decoded PCM frame is available (after SRTP decrypt + Opus decode + 3A post-process on the receive path) | `void(const float* pcm, size_t num_samples)` |
| `on_video_frame` | A decoded H.264 NAL is available on the receive path (pre-render) | `void(const uint8_t* nal, size_t len, bool keyframe)` |
| `on_error` | Any subsystem error fires (transport, DTLS, audio3A, codec, scheduler) | `void(uint32_t err, std::string_view msg)` |
| `on_state_change` | Engine state transitions (e.g. ICE → connected) | `void(const char* state)` |
| `on_data_message` | A DataChannel message arrives | `plugins::DataMessageCallback` |
| `on_data_state` | A DataChannel state transition (open / closed / error) | `plugins::DataChannelStateCallback` |

> **Threading**: callbacks fire on the engine thread (the thread
> that called `open()` and drives `tick()`). Plugins may also call
> `on_error` from the ICE recv path; check per-family docs.

---

## 5. SDP

### 5.1 `create_offer()`

```cpp
std::string create_offer() noexcept;
```

Generates an RFC 8829-conformant SDP offer. Includes media
section(s) for audio and (if enabled) video. The offer is local:
it contains `c=IN IP4 0.0.0.0` and the engine's own ICE
credentials; the answerer reads them and routes its STUN
accordingly.

### 5.2 `process_remote_sdp()`

```cpp
std::optional<std::string> process_remote_sdp(std::string_view remote_sdp) noexcept;
```

Parses the remote SDP, validates it against the engine's
configuration (codec / transport compatibility), and produces the
matching answer. Returns `nullopt` if parsing fails; the error
fires via `on_error_`.

Once both sides have processed each other's SDP, ICE connectivity
checks begin. Loopback Case D, cross-Chrome, and the agent
gateway profile all use this exact flow.

---

## 6. ICE inspection & control

### 6.1 `ice_state_string()`

```cpp
const char* ice_state_string() const noexcept;
```

Returns one of `"disconnected"`, `"gathering"`, `"checking"`,
`"connected"`, `"completed"`, `"failed"`, `"closed"`. Convenient
for logging and `std::string(...)` interpolations.

### 6.2 `local_ufrag()`, `local_password()`

```cpp
std::string local_ufrag()    const noexcept;
std::string local_password() const noexcept;
```

The local ICE credentials. They are auto-included in our SDP
offer; use these accessors only if you're building a non-SDP
signaling path (e.g. custom JSON-only signaling).

### 6.3 `set_remote_ice()`

```cpp
bool set_remote_ice(std::string_view ice_block) noexcept;
```

Inject the peer's `a=ice-ufrag` / `a=ice-pwd` / `a=candidate`
lines into the local ICE agent. **Must** be called before `open()`
(or between `pre_open()` and `open()`) for the answerer; otherwise
libjuice defaults to `controlling`, and role conflict prevents
connectivity.

### 6.4 `add_turn_server()`

```cpp
int add_turn_server(std::string_view host, std::uint16_t port,
                    std::string_view username,
                    std::string_view password) noexcept;
```

Adds a TURN relay for candidate gathering. Requires `pre_open()`
first. Returns 0 on success; non-zero if the ICE transport is not
yet created.

---

## 7. DTLS / SRTP inspection

```cpp
nimrtc::dtls::DtlsState dtls_state() const noexcept;
bool                    dtls_connected() const noexcept;
bool                    srtp_installed() const noexcept;
std::string             local_dtls_fingerprint_sha256_base64() const noexcept;
```

The DTLS state machine runs from `Closed` → `HelloSent` →
`Connected` (`dtls::DtlsState` enum). The engine auto-installs SRTP
keys once `dtls_connected()` is true; `srtp_installed()` exposes the
post-install flag for diagnostics.

`drain_dtls()` and `maybe_install_srtp_keys()` are called by `tick()`
in the background; no manual invocation is required.

`local_dtls_fingerprint_sha256_base64()` returns the SPKI SHA-256
fingerprint in the WebRTC base64 form (matches Chrome's
`--ignore-certificate-errors-spki-list`).

---

## 8. Audio I/O

```cpp
uint32_t send_audio(const float* pcm_samples, size_t num_samples) noexcept;
```

Encode `pcm_samples` with the configured audio codec, RTP-packetise
it, SRTP-encrypt it, and submit to the scheduler. The buffer must
remain valid for the duration of the call (no copy made).

Call cadence: `num_samples / (sample_rate_hz * channels)` matches
the frame duration in seconds; typical 10 ms / 20 ms Opus frames.
The engine handles framing internally as long as you keep the
cadence consistent — opportunistic re-pacing happens in `tick()`.

Returns `kOk` on success; non-zero on encode / encrypt failure
(surfaces via `on_error_`).

---

## 9. Video I/O

### 9.1 `start_video()` / `stop_video()`

```cpp
plugins::Status start_video() noexcept;
void            stop_video()  noexcept;
```

Start / stop the configured `video_source_name`. Idempotent.

### 9.2 `send_video()`

```cpp
plugins::Status send_video(const plugins::VideoSourceFrame& frame) noexcept;
```

Manually push one raw frame through the pipeline. Used for feeding
camera frames directly when the configured `IVideoSource`
(`memory` default) doesn't drive its own capture loop. The frame
is encoded, RTP-packetised, and enqueued to the scheduler.

---

## 10. DataChannel control plane

```cpp
std::unique_ptr<plugins::IDataChannel>
create_data_channel(std::string_view label) noexcept;
```

Allocates a new `IDataChannel` from the resolved factory; opens it
with a default config (label, ordered, reliable, priority 128).
Returns `nullptr` if:

- `cfg.datachannel_name.empty()`;
- the factory id was not registered (caller forgot
  `core::register_all_default_plugins()`);
- the factory's `create()` returned null (catastrophic impl bug).

Must be called after `open()`. The returned channel is owned by the
caller; pass it to a transport-stack or use it directly via the
`IDataChannel` interface.

`set_on_data_message()` and `set_on_data_state()` install the
matching callbacks on **every** new channel created after the call.

> **Status**: v0.11.0 satisfies DC-1 Gate #1 via the §7 fallback
> (in-process `test_datachannel_engine` 5/5 PASS).
> Physical SCTP-over-DTLS plumbing is the v0.11.x follow-up;
> §engine.hpp comment `v0.11.x follow-up` explains the caveat.

---

## 11. Tick loop

```cpp
int tick() noexcept;
```

The single user-facing "drive everything forward" call. Each
invocation:

- drains the ICE socket and dispatches RTP / DTLS / STUN packets;
- feeds inbound RTP into the jitter buffer;
- runs the decoder for any playable frames;
- fires scheduled outbound frames from the BWE / scheduler;
- pumps the DTLS retransmit timer (~50 ms cadence is sufficient).

Typical cadence: 10 ms to 33 ms. The engine tolerates a missed
tick; long stalls just delay outbound packets.

Returns the number of packets dispatched in this tick.

---

## 12. Plugin accessors

Every accessor returns a non-owning pointer or a `const` reference.
Pointers can be `nullptr` if the configured id was not registered
**or** if `open()` was not called yet.

| Accessor | Returns | Useful for |
|---|---|---|
| `transport()` / `ice_transport()` | `plugins::ITransport*` / `IICETransport*` | Direct ICE inspection / extra candidate listening. |
| `audio3a()` | `IAudio3A*` | Setting up pre/post-3A PCM taps for ASR. See `examples/demo-agent-gateway`. |
| `codec_plugin()` | `ICodec*` | Codec-specific statistics / introspection. |
| `video_source()` / `video_sink()` | `IVideoSource*` / `IVideoSink*` | Running your own capture / render loops outside the engine frame cadence. |
| `video_receiver()` / `video_sender()` | `IVideoReceiver*` / `IVideoSender*` | RTP-level instrumentation (NACK, FU-A). |
| `video_codec()` | `IVideoCodec*` | H.264 hardware vs software selection. |
| `bwe()` | `IBwe*` | Reading BWE stats (current estimate, loss rate). |
| `scheduler()` | `IScheduler*` | Reading scheduler queue depth. |
| `config()` | `const EngineConfig&` | Re-reading effective cfg (e.g. PCM rate for ASR sidecar). |

### Runtime swaps

```cpp
void set_video_sink(std::unique_ptr<plugins::IVideoSink> sink) noexcept;
void set_video_source(std::unique_ptr<plugins::IVideoSource> source) noexcept;
```

Replace the plugin instance at runtime (e.g. swap `"headless"` for
an SDL renderer). Closes the previous instance, transfers ownership
of the new one. Pass `nullptr` to detach.

---

## 13. Error codes & inspection

```cpp
uint32_t last_open_rc() const noexcept;

struct Stats {
    int srtp_drops = 0;
};
Stats stats() const noexcept;
```

`last_open_rc()` returns the last error code from `open()` /
`pre_open()`. Useful in tests when the bool result is non-OK but
you want to know which subsystem failed:

| Bit region | Subsystem |
|---|---|
| `0x2000` | DTLS |
| `0x1FFF` | ICE / transport |
| `0x1A00` | audio3A |
| `0x1800` | codec |
| `0x1900` | scheduler |
| `0x1700` | JB |

`Stats::srtp_drops` increments on every SRTP auth-tag mismatch.

---

## 14. Lifecycle diagram

```
┌─────────────────────────────┐
│ EngineConfig cfg;           │
│ NimRTCEngine engine(cfg);   │  ctor — no I/O yet
└─────────────────────────────┘
                │
                ▼
        set_on_audio_frame(...)
        set_on_error(...)
        set_on_state_change(...)
                │
                ▼
   ┌─── pre_open() ───┐    (optional)
   │ set_remote_ice() │
   └────────┬─────────┘
            ▼
        open() ──────────────────────────────────────┐
            │                                        │
            ▼                                        │
   while (tick() && !connected) {                    │
       tick();                                       │
       std::this_thread::sleep_for(10ms);            │
   }                                                 │
            │                                        │
            ▼                                        │
   create_offer() ──► answer                          │
   process_remote_sdp(answer)                        │
            │                                        │
            ▼                                        │
   send_audio(pcm);                                  │
            │                                        │
            ▼                                        │
   close()    ◄──── (always, even on error)          │
                                                    err
```

---

## 15. Threading model

- The engine serialises all of its work on the **caller's thread**
  (the thread that called `open()` and drives `tick()`).
- Plugins called from `tick()` may dispatch to worker threads only
  if their interface documentation explicitly allows it.
- The audio / video frame callbacks fire on the engine thread.
- The DataChannel callbacks fire on the engine thread.

> **Implication**: if you bind the same `IDataChannel::set_on_message`
> callback from two engines, you'll get calls from two threads.
> Protect with a mutex.

---

## 16. See also

- `engine_config.md` — full field reference for `EngineConfig`.
- `plugin_author_guide.md` — how to write your own plugin (DTLS,
  audio3A, video, BWE).
- `examples/loopback-p2p/loopback-p2p.cpp` — full minimal program.
- `examples/demo-agent-gateway/` — Profile + PCM tap demo.
- `src/engine/include/nimrtc/engine/engine.hpp` — header.
- ADR-001 (plugin seam), ADR-009 (PAL Slice 1).

> API is **frozen at the source-level for v0.12.0**. Additions come
> in `[[deprecated]]`-marked overloads. Source-breaking changes go
> through a deprecation cycle (see
> `docs/zh/architecture.md` §12).
