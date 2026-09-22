# `EngineConfig` — Field Reference

> Header: `src/engine/include/nimrtc/engine/engine.hpp`
> Status: Public API for v0.12.0-alpha. Additive changes (new
> fields with defaults) are non-breaking; renames go through a
> deprecation cycle.

`EngineConfig` is the **single config bag** you pass to
`NimRTCEngine`. Every field has a default; you only set what you
want to override. Most fields are string IDs that select which
plugin a capability resolves to.

---

## 1. Quick reference

```cpp
EngineConfig
├── Plugin selection (string IDs)
│   ├── transport_name                  = "ice"
│   ├── rtp_name                        = "webrtc"
│   ├── sdp_name                        = "webrtc"
│   ├── jb_name                         = "adaptive"
│   ├── audio3a_name                    = "webrtc_apm"
│   ├── codec_name                      = "opus"
│   ├── dtls_name                       = ""  → "wolfssl"
│   ├── bwe_name                        = "aimd"
│   ├── scheduler_name                  = "strict_priority"
│   ├── video_codec_name                = "h264"
│   ├── datachannel_name                = "sctp"
│   ├── video_source_name               = "memory"
│   ├── video_sink_name                 = "headless"
│   ├── video_receiver_name             = "reference"
│   └── video_sender_name               = "reference"
│
├── Audio codec advertisement (SDP / send_audio)
│   └── audio_codec: AudioCodecConfig
│
├── Network
│   ├── local_bind_address              = ""
│   ├── stun_server_host / _port        = ""
│   ├── local_port_range_begin / _end   = 0
│   └── pcm_sample_rate_hz / pcm_channels = 48000 / 1
│
├── Jitter buffer
│   ├── jb_initial_delay_ms             = 40
│   ├── jb_min_delay_ms                 = 10
│   └── jb_max_delay_ms                 = 200
│
├── Video plugin tuning
│   ├── video_receiver_tuning: VideoReceiverTuning
│   └── video_sender_tuning:   VideoSenderTuning
│
├── BWE / Scheduler tuning
│   ├── bwe_config: plugins::BweConfig
│   └── scheduler_config: plugins::SchedulerConfig
│
├── DataChannel
│   └── sctp_port                       = 5000
│
└── Audio3A note: set audio3a_name = "" to disable 3A entirely
```

---

## 2. Plugin selection fields

Each field is a `std::string_view` (or `std::string` for fields that
may be left empty to mean "default"). The string is a stable id
registered via `core::PluginRegistry::register_<family>()` or
`NIMRTC_REGISTER_<FAMILY>(name, &factory)`. For guidance on
implementing your own plugin see `docs/plugin_author_guide.md`.

If a configured id is not registered when `open()` runs, the engine
returns a non-zero error code via `last_open_rc()` and fires
`on_error_`. **There is no silent fallback.** This is intentional:
configuration typos must surface immediately.

### 2.1 Wire / transport

| Field | Default | Effect |
|---|---|---|
| `transport_name` | `"ice"` | `ITransport` plugin id (ICE-based). See §6.1 below. |
| `rtp_name` | `"webrtc"` | `IRTP` plugin id. |
| `sdp_name` | `"webrtc"` | `ISDP` plugin id. |
| `jb_name` | `"adaptive"` | `IJB` plugin id. |

### 2.2 DTLS backend

```cpp
std::string dtls_name;   // default = "" → resolved to "wolfssl"
```

Sets the DTLS backend by id. Built-in registrations:

- `""` → `"wolfssl"` (v0.10.x default; preserved for source compat).
- `"wolfssl"` — `DtlsSessionWolfSSL` (P-256 ECDSA, AES-128-GCM, SHA-256).
- `"gmssl"` — `DtlsSessionGmSSL` (GM/T 0044 TLCP, SM2/SM3/SM4 / SM4-GCM).
  Requires `NIMRTC_ENABLE_DTLS_GMSSL=ON` at configure time.

See [switching-dtls-backend.md](../guides/switching-dtls-backend.md)
for the full switching guide and constraints.

### 2.3 Audio pipeline

| Field | Default | Effect |
|---|---|---|
| `audio3a_name` | `"webrtc_apm"` | `IAudio3A` plugin id. Set to `""` to disable 3A (raw passthrough). |
| `codec_name` | `"opus"` | `ICodec` plugin id. Empty = first statically-linked codec. |
| `audio_codec` | RFC 7587 Opus | Audio codec parameters advertised in SDP and used for `send_audio()`. See §3. |

> The engine consumes `audio3a_name` first; when the prebuilt
> WebRTC APM is missing (`NIMRTC_VENDORED_WEBRTC_APM=OFF` /
> `.a` not on disk), the factory falls back to `NullAudio3A` and
> `engine.audio3a()` may return `nullptr`. To force the fallback
> explicitly, configure `audio3a_name = "webrtc"` (the built-in
> null stub id).

### 2.4 BWE / Scheduler

```cpp
std::string bwe_name       = "aimd";             // IBwe plugin
std::string scheduler_name = "strict_priority";  // IScheduler plugin
```

Empty + `bwe_config.enabled == false` (or
`scheduler_config.enabled == false`) opts the engine out of BWE /
scheduling (used by the `transport` profile for pure passthrough).

### 2.5 Video plugins

The video plugins are resolved but **not yet threaded into the
RTP/DTLS/SRTP pipeline** in v0.12.0 (P1.1 future work). Callers
drive them through the accessors
(`engine.video_receiver()->push_rtp(pkt, now_us)`).

```cpp
std::string_view video_source_name     = "memory";
std::string_view video_sink_name       = "headless";
std::string_view video_receiver_name   = "reference";
std::string_view video_sender_name     = "reference";
```

To select HW backends:

```cpp
cfg.video_source_name     = "v4l2";          // Linux
cfg.video_sender_name     = "mediacodec";    // Android NDK
cfg.video_receiver_name   = "videotoolbox";  // Apple
```

### 2.6 DataChannel

```cpp
std::string datachannel_name = "sctp";   // empty = no DataChannel
std::uint16_t sctp_port      = 5000;
```

`datachannel_name = ""` disables DataChannel entirely:

- `create_offer()` does **not** emit `m=application`;
- `process_remote_sdp()` silently drops any `m=application` from
  the peer;
- `create_data_channel()` returns `nullptr`.

### 2.7 Video codec

```cpp
std::string video_codec_name = "h264";   // IVideoCodec plugin id
```

Empty = no video codec. The codec is only consumed when the video
pipeline is active (currently not threaded end-to-end).

---

## 3. Audio codec advertisement

```cpp
struct AudioCodecConfig {
    std::uint8_t  payload_type = 111;     // RFC 7587 default
    std::string   encoding     = "opus";  // "opus" | "PCMU" | "PCMA"
    std::uint32_t clock_rate   = 48000;
    std::uint8_t  channels     = 2;
    std::string   fmtp;                   // e.g. "minptime=10;useinbandfec=1"
};
```

Used in two places:

1. `create_offer()` emits an `a=rtpmap:<pt> <encoding>/<rate>/<ch>`
   line and an `a=fmtp:<pt> <fmtp-string>` line when `fmtp` is
   non-empty.
2. `send_audio(pcm, n)` forwards `n` samples per channel; the engine
   computes frame duration from `clock_rate` and `channels`.

For Opus interop with Chrome / Firefox / Safari, leave defaults.
For **narrowband fallback** (when Opus isn't linked, e.g.
`NIMRTC_HAS_OPUS` undefined), use:

```cpp
cfg.audio_codec = AudioCodecConfig{
    .payload_type = 0,
    .encoding     = "PCMU",
    .clock_rate   = 8000,
    .channels     = 1,
};
```

`PCMA` (alaw, `pt=8`) follows the same shape.

---

## 4. Network

```cpp
std::string   local_bind_address;        // "" = "0.0.0.0"
std::string   stun_server_host;          // "" = skip STUN gathering
std::uint16_t stun_server_port = 3478;
std::uint16_t local_port_range_begin = 0;
std::uint16_t local_port_range_end   = 0;
```

### 4.1 Port ranges

`local_port_range_begin` / `_end` define the UDP socket pool the
ICE transport binds. Both `0` = OS picks. **When you run multiple
`NimRTCEngine` instances on the same host (e.g. loopback demo), each
must have a unique port range** — otherwise they steal each other's
STUN packets.

The loopback-p2p example uses `51000–51099`; demo-agent-gateway uses
`52000–52099`. Pick non-overlapping ranges in your own code.

### 4.2 TURN

TURN servers are configured at runtime via
`engine.add_turn_server(host, port, user, pass)` — not in the
config struct, because TURN servers are typically per-session
(rotated by an SFU / TURN service).

---

## 5. Audio frame parameters

```cpp
std::uint32_t pcm_sample_rate_hz = 48000;
std::uint8_t  pcm_channels       = 1;
```

These are the dimensions of the buffer you pass to `send_audio()`.
The audio codec itself (Opus / PCMU) may resample; `clock_rate` /
`channels` in `audio_codec` describe **the codec**, while
`pcm_sample_rate_hz` / `pcm_channels` describe **the input PCM**.

Common combinations:

| Capture | Codec cfg | Notes |
|---|---|---|
| 48 kHz mono mic | `audio_codec = Opus 48k stereo`; `pcm_* = 48000 / 1` | engine downsamples / mixes. |
| 16 kHz mono mic | `audio_codec = Opus 48k stereo`; `pcm_* = 16000 / 1` | engine upsamples. |
| 8 kHz mono mic | `audio_codec = PCMU 8k mono`; `pcm_* = 8000 / 1` | Alaw interop fallback. |

---

## 6. Jitter buffer

```cpp
int jb_initial_delay_ms = 40;
int jb_min_delay_ms     = 10;
int jb_max_delay_ms     = 200;
```

Tune for your use case:

| Use case | initial | min | max |
|---|---|---|---|
| VoIP / calling (`call` profile) | 40 | 10 | 200 |
| Live broadcast (`live` profile) | 80 | 20 | 500 |
| AI Agent (`agent` / `agent-gateway`) | 30 | 10 | 150 |
| Teleop (`teleop` profile) | 20 | 5 | 100 |

---

## 7. Video plugin tuning

```cpp
struct VideoReceiverTuning {
    std::uint32_t ssrc                 = 0xDEADBEEF;
    std::uint8_t  payload_type         = 102;
    std::uint32_t max_inflight_frames  = 8;
    std::uint32_t max_jitter_buffer_ms = 200;
    bool          emit_nacks           = true;
    bool          expect_fu_a          = true;
} video_receiver_tuning;

struct VideoSenderTuning {
    std::uint32_t ssrc         = 0xCAFEBABE;
    std::uint8_t  payload_type = 102;
    std::uint16_t mtu          = 1200;
    std::uint16_t initial_seq  = 0;
} video_sender_tuning;
```

Forwarded to `IVideoReceiver` / `IVideoSender` plugin factories.
Defaults match RFC 5109 + WebRTC interop. For HW pipelines (e.g.
MediaCodec + Android Camera2) you typically override `ssrc` and
`payload_type` per session.

> SSRC collision check is the engine's responsibility in v0.12.0+
> (per-peer uniqueness); these defaults are deliberate collisions
> so accidental reuse is easy to spot in pcap dumps.

---

## 8. BWE / Scheduler config

```cpp
plugins::BweConfig       bwe_config;        // initial bitrate, gain, caps, ...
plugins::SchedulerConfig scheduler_config;  // queue caps, weights, ...
```

Configure parameters forwarded to your chosen BWE / Scheduler
factory's `create()`. To disable either subsystem entirely (used by
the `transport` profile for pure passthrough):

```cpp
cfg.bwe_config.enabled = false;
// (leave bwe_name = "aimd" — the engine just doesn't construct one)
cfg.scheduler_config.enabled = false;
```

---

## 9. DataChannel port

```cpp
std::uint16_t sctp_port = 5000;
```

The SCTP port advertised in `m=application` lines. Defaults to
`5000` to match Chrome / WebRTC interop. Only used when
`datachannel_name` is non-empty.

---

## 10. Decision matrix — which plugin to select?

| I want to… | Set these | Don't touch |
|---|---|---|
| Default 1:1 voice call | (defaults) | — |
| Embed on aarch64 with wolfSSL | `dtls_name = "wolfssl"` (already default) | `audio3a_name`, `codec_name` |
| Use GM/T cryptography (OpenHarmony) | `dtls_name = "gmssl"` | — |
| Skip 3A, send raw PCM | `audio3a_name = ""` | — |
| Disable SRTP encryption | not possible via config; DTLS is mandatory for SRTP key derivation. |
| Use a custom ICE transport (QUIC, custom) | `transport_name = "myicetransport"`; ensure factory is registered before `open()` | `dtls_name`, `audio3a_name` |
| Run a 16 kHz mic → Opus stream | `pcm_sample_rate_hz = 16000; pcm_channels = 1;` (rest unchanged) | `codec_name` |
| Pure DTLS-SRTP passthrough (transport profile) | `bwe_config.enabled = false; scheduler_config.enabled = false;` | `audio3a_name = ""; codec_name = "";` |

---

## 11. See also

- `engine_api.md` — public method reference.
- `docs/plugin_author_guide.md` — implement your own plugin.
- `docs/guides/switching-dtls-backend.md` — DTLS swap guide.
- `src/engine/include/nimrtc/engine/engine.hpp` — header.
- ADR-001 (plugin seam), ADR-009 (PAL Slice 1).

> **Validation note**: writing config docs is the cheapest way to
> find API design bugs. If this document disagrees with the
> header, the header is the source of truth — file an issue with
> `[docs]` prefix.
