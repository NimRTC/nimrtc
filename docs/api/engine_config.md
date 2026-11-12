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
│   ├── datachannel_backend             = "web_transport"  ← new in v0.14.0-rc.1
│   ├── datachannel_name                = ""             (legacy factory slot)
│   ├── web_transport_name              = "msquic"       ← new in v0.14.0-rc.1
│   ├── video_source_name               = "memory"
│   ├── video_sink_name                 = "headless"
│   ├── video_receiver_name             = "reference"
│   └── video_sender_name               = "reference"
│
├── Audio codec advertisement (SDP / send_audio)
│   └── audio_codec: AudioCodecConfig
│
├── Audio pipeline (receive path)                    ← new in v1.2.0
│   ├── audio_decoder_name              = "opus"
│   ├── audio_receiver_name             = "nimrtc_audio_rx"
│   ├── audio_ptime_ms                  = 20
│   ├── audio_use_inband_fec            = true
│   ├── audio_use_dtx                   = false
│   ├── audio_jitter_target_delay_ms    = 40
│   ├── audio_max_concealed_frames      = 10
│   ├── audio_downmix_to_pcm_channels   = true
│   └── audio_redundancy_percent        = 5
│
├── Adaptive audio bitrate (BWE-linked)
│   ├── audio_bitrate_bps               = 0            ← 0 = codec default
│   ├── audio_min_bitrate_bps           = 8000
│   ├── audio_bitrate_adaptive          = true
│   ├── audio_bwe_share                 = 0.1
│   └── audio_bitrate_update_interval_ms = 1000
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
| `audio_decoder_name` | `"opus"` | `IAudioDecoder` plugin id — the decoder of the **receive** path. |
| `audio_receiver_name` | `"nimrtc_audio_rx"` | `IAudioReceiver` plugin id — the receive pipeline itself. |
| `audio_ptime_ms` | `20` | Audio frame duration in ms (10 / 20 / 40 / 60). Also the SDP `ptime`. |
| `audio_use_inband_fec` | `true` | Opus in-band FEC. Must match the `a=fmtp` we send. |
| `audio_use_dtx` | `false` | Opus DTX — only honoured when the peer signals `usedtx=1`. |
| `audio_jitter_target_delay_ms` | `40` | Target jitter-buffer depth for the audio stream. |
| `audio_max_concealed_frames` | `10` | Maximum number of consecutive concealed frames before the pipeline gives up on the gap. |
| `audio_downmix_to_pcm_channels` | `true` | Downmix the decoded stream to `pcm_channels`. |
| `audio_redundancy_percent` | `5` | In-band redundancy the encoder embeds, in percent (0..100). See below. |

> The engine consumes `audio3a_name` first; when the prebuilt
> WebRTC APM is missing (`NIMRTC_VENDORED_WEBRTC_APM=OFF` /
> `.a` not on disk), the factory falls back to `NullAudio3A` and
> `engine.audio3a()` may return `nullptr`. To force the fallback
> explicitly, configure `audio3a_name = "webrtc"` (the built-in
> null stub id).

**The receive path has no fallback.**  `audio_decoder_name` and
`audio_receiver_name` name real registry slots: a missing decoder
plugin or a missing receive pipeline is a hard error
(`kAudioDecoderMissing` / `kAudioReceiverMissing`) that fires
`on_error` exactly once and disables the audio track.  The inbound
packets are then counted and dropped — never decoded.  This is
deliberate: a "built-in minimal receiver" would decode nothing and
hand the bit-stream to the host as if it were audio.

**`audio_redundancy_percent`** is the in-band redundancy (the SILK
LBRR copy of the *previous* frame that the encoder embeds in every
frame).  It is handed to the encoder, so every frame carries the copy,
and to the decoder, which uses it as its own redundancy hint.  A
receiver that lost a frame reconstructs it from the redundancy the
*next* packet carries.  **0** disables the redundancy — the encoder
then carries no copy at all and the FEC recovery path cannot work.

#### 2.3.1 Adaptive audio bitrate

| Field | Default | Effect |
|---|---|---|
| `audio_bitrate_bps` | `0` | Target bitrate of the audio encoder. 0 = codec default: **32 kbps for mono** (the default `pcm_channels` is 1), 64 kbps for stereo. Also the ceiling of the adaptation. |
| `audio_min_bitrate_bps` | `8000` | Floor of the adaptation — the encoder never goes below it.  A peer's TMMBR limit overrides it (see below). |
| `audio_bitrate_adaptive` | `true` | Enable the BWE-linked adaptation. `false` pins the encoder at `audio_bitrate_bps`.  A peer's TMMBR limit is honoured regardless. |
| `audio_bwe_share` | `0.1` | Share of the bandwidth estimate the audio encoder may use (the rest belongs to video). |
| `audio_bitrate_update_interval_ms` | `1000` | Minimum interval between two adaptation steps. |

The controller runs from `tick()` and reads the BWE estimate:

```
budget = estimate.target_bitrate_bps * audio_bwe_share
target = clamp(budget, audio_min_bitrate_bps, audio_bitrate_bps)
```

The step towards the target is rate-limited, so the encoder never
oscillates:

- a **drop** is applied immediately — at most **-50 %** per step, so a
  congestion collapse is answered within a couple of intervals;
- a **rise** is at most **+10 %** per step, so a flickering estimate
  cannot pump the encoder up and down;
- a deviation of less than **10 %** is ignored altogether (hysteresis).

The bitrate is pushed into the codec through `ICodec::update_config()`,
which reconfigures the *running* encoder in place — the codec state and
its statistics survive the step, so an adaptation never interrupts the
stream.

The effective bitrate is observable through `Stats::audio_encoder_bitrate_bps`.

##### The peer's TMMBR limit outranks the floor

A TMMBR the peer sends (RFC 5104 section 4.2.1.2) caps the encoder as
well, and the cap **overrides `audio_min_bitrate_bps`**.  The two
constraints are not of the same kind:

- `audio_min_bitrate_bps` exists so that a *congestion collapse* cannot
  starve the audio — it protects the stream against the network.
- The peer's limit is a *promise*: the engine answers the TMMBR with a
  TMMBN (RFC 5104 section 4.2.2.2), and that acknowledgement tells the
  peer "this bound is in force".  A commitment to send *less* is not
  something the sender may quietly exceed, so it wins over the local
  floor — the encoder is allowed to drop **below** `audio_min_bitrate_bps`
  when the peer asks for less.

The same limit clamps the scheduler budget and the pacing rates, so the
promise holds end to end: the encoder, the pacer and the budget all
stay under it.  It is honoured even when `audio_bitrate_adaptive` is
`false` — that switch says "do not move the encoder because of the
*estimate*", not "break the promises the engine made on the wire".

### 2.4 BWE / Scheduler

```cpp
std::string bwe_name       = "aimd";             // IBwe plugin
std::string scheduler_name = "strict_priority";  // IScheduler plugin
```

Empty + `bwe_config.enabled == false` (or
`scheduler_config.enabled == false`) opts the engine out of BWE /
scheduling (used by the `transport` profile for pure passthrough).

### 2.4a RTCP Extended Report consumer

```cpp
std::string xr_report_name = "nimrtc_xr";   // IXrReport plugin
```

Selects the consumer that decodes the peer's RFC 3611 extended
reports (RTCP payload type 207).  The engine demultiplexes every
inbound RTCP compound datagram and routes the XR sub-packets into it;
the host reads the decoded blocks out of `engine.xr_report()->snapshot()`.

An **empty string disables the consumer** — the XR sub-packet is then
recognised by the demultiplexer and dropped, which is the pre-v1.2.0
behaviour.

The blocks NimRTC parses are **BT=4** (Receiver Reference Time),
**BT=5** (DLRR) and **BT=6** (Statistics Summary); the other block
types RFC 3611 registers are recognised but not parsed (they are
skipped by their length, never an error).  See
[`engine_api.md` §8.6](./engine_api.md) for the full matrix and for
the note on the draft-vs-RFC block numbering.

### 2.4b RTCP XR / TMMBR sending

```cpp
bool rtcp_xr_enabled = false;   // send RFC 3611 extended reports
bool tmmbr_enabled   = false;   // send RFC 5104 TMMBR feedback
bool disable_tmmbr   = false;   // suppress TMMBR entirely (overrides the above)
```

These select the **outbound** half of the RTCP feedback surface.  All
three default to `false`: an XR report is a QoE / network-statistics
report and a TMMBR changes how the peer sends, so a host that never
asked for either should not get it.  `disable_tmmbr` overrides
`tmmbr_enabled` — a host that sets both gets no TMMBR.

**`rtcp_xr_enabled`.**  The engine emits one RFC 3611 extended report
per RTCP interval, carrying the Receiver Reference Time block (BT=4),
the DLRR block (BT=5) that answers the peer's own reference-time
report, and the Statistics Summary block (BT=6).  The report is
**negotiated**: the offer carries `a=rtcp-xr:rcvr-rtt=all
stat-summary=loss,jitt` and the engine sends nothing until the peer's
description named the format (RFC 3611 section 5.1).

The `stat-summary` field list is the *peer's*: RFC 3611 section 5.1
says it "contains a list indicating which fields SHOULD be included in
the Statistics Summary report blocks that are sent", so the block's L
and J flag bits follow whatever the peer asked for.  A peer that asked
for `loss` alone gets no jitter flag.

**`tmmbr_enabled`.**  The engine emits a TMMBR carrying the bit rate
its bandwidth estimator is *currently using* — the number the peer
should throttle to.  It is negotiated through
`a=rtcp-fb:<pt> ccm tmmbr` (RFC 5104 section 7.2), so the engine
sends no TMMBR unless the peer's description carried the parameter
too.

**`disable_tmmbr`.**  Vetoes the feature in both directions: no TMMBR
is sent, and the SDP carries no `tmmbr` parameter.  It overrides
`tmmbr_enabled`.

The *inbound* side needs no configuration: a TMMBR the peer sends is
always honoured — the engine clamps its scheduler budget, its pacing
rates **and its audio encoder** to `peer_tmmbr_limit_bps()` and answers
with a TMMBN (RFC 5104 section 4.2.2.2).  See
[`engine_api.md` §8.7](./engine_api.md) for the details.

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
std::string datachannel_backend = "web_transport"; // "web_transport" | "" ("sctp" retired in v1.2.0)
std::string datachannel_name    = "";    // legacy factory slot; empty = no DataChannel
std::string web_transport_name  = "msquic"; // WebTransport session factory id
std::uint16_t sctp_port        = 5000;

// v1.2.0 (DataChannel capability plan WP-B / WP-E1 / WP-D4):
plugins::DataChannelFraming datachannel_framing = plugins::DataChannelFraming::kRaw;
std::uint8_t datachannel_priority = 128;
std::uint16_t datachannel_max_retransmits = 0;          // 0 = unlimited
std::uint16_t datachannel_max_packet_lifetime_ms = 0;   // 0 = unlimited
```

**v0.14.0-rc.1** added the `datachannel_backend` + `web_transport_name`
fields to support the DC-1 v1.0.0 headline gate (WebTransport / QUIC
data plane; see `docs/plan/v1.0-plan.md` §2.1.1 PR-3).

**v1.2.0** retired the `"sctp"` backend from the public selectors and
changed the default of `datachannel_name` from `"sctp"` to `""`
(`docs/plan/datachannel-capability-plan.md` WP-A, DEC-4 = A1).

#### 2.6.0 Which backends can actually reach a peer

| Backend | Reaches a remote peer? | Notes |
|---|---|---|
| `"web_transport"` (default) | ✅ yes | WebTransport / QUIC on its own connection (separate from the ICE/DTLS media 5-tuple). This is the production data plane. |
| `"sctp"` | ❌ **no** — **rejected** | **Retired in v1.2.0** (`docs/plan/datachannel-capability-plan.md` WP-A / DEC-4 = A1). The SCTP factory opens an in-process usrsctp socket bound to `127.0.0.1` (`src/sctp/src/usrsctp_socket.cpp`); the engine never wires SCTP-over-DTLS, so the backend could never reach a peer. Passing `"sctp"` now fails `open()` with `kEngineInternal` and an error message that says it is test-only. |

The SCTP factory stays registered in `core::PluginRegistry` as the
in-tree **reference implementation** for the DCEP codec and the
PR-SCTP semantics — the module tests drive it directly.  What was
retired is the *public selector*, not the backend.

#### 2.6.1 Backend dispatch

The engine resolves `core::PluginRegistry::get_datachannel(<id>)` at
`init_modules_once()` time by dispatching on `datachannel_backend`:

| `datachannel_backend` | Resolved factory id       | Notes                                           |
|-----------------------|---------------------------|-------------------------------------------------|
| `"web_transport"` (default) | `"web_transport"` (fixed; `datachannel_name` is ignored) | Production path — the only backend with a wire path. Also verifies `web_transport_name` session factory (default `"msquic"`). |
| `"sctp"`              | **(hard error)**          | Retired in v1.2.0 — surfaces `kEngineInternal` via `on_error_`; the message names the backend as test-only (§2.6.0). |
| `""`                  | `datachannel_name` (legacy v0.x fallback) | Backward compat — a v0.x caller that set only `datachannel_name` continues to work without code changes. |
| `<other>`             | **(hard error)**          | Surfaces `kEngineInternal` via `on_error_` — engine does NOT silently fall back to another backend (typos would mask integrator errors). |

The legacy spelling reaches the same retired family: `datachannel_backend`
empty plus `datachannel_name = "sctp"` is rejected exactly like
`datachannel_backend = "sctp"`.

#### 2.6.2 WebTransport session factory

When `datachannel_backend = "web_transport"`, the engine resolves
`core::PluginRegistry::get_web_transport(web_transport_name)`
(default `"msquic"`) at `init_modules_once()` time.  A missing
session factory is a soft-warn (logged via `core::log`); the failure
surfaces later, when `create_data_channel()` tries to create the
session.

The physical session is created **lazily, on the first
`create_data_channel()` call**, and is then **shared by every channel
of that engine** (`Impl::wt_session`).  The engine therefore needs
`web_transport_peer_host` (and, for a non-default port,
`web_transport_peer_port`) to be set before the first channel is
created — an empty peer host means in-process loopback (unit tests).

#### 2.6.3 Disabling DataChannel

Set `datachannel_backend = ""` to disable DataChannel entirely:

- `create_offer()` does **not** emit `m=application`;
- `process_remote_sdp()` silently drops any `m=application` from
  the peer;
- `create_data_channel()` returns `nullptr`.

In the legacy path (where `datachannel_backend` is empty),
`datachannel_name = ""` is the equivalent switch (it is the
authoritative knob there, mirroring the v0.x contract).  **In the
default `"web_transport"` family `datachannel_name` is ignored**, so
clearing it alone does *not* disable DataChannel — clear
`datachannel_backend` as well.

Note that in the `"web_transport"` family the engine emits **no**
`m=application` line at all: the data plane rides its own QUIC
connection rather than the ICE/DTLS 5-tuple.  An offer without
`m=application` therefore does not by itself mean "DataChannel
disabled".

#### 2.6.4 Power-user factory override (legacy v0.x)

A v0.x caller that set `datachannel_name = "<custom-id>"` (a value
other than `"sctp"`) continues to resolve that id directly through
`core::PluginRegistry::get_datachannel("<custom-id>")` — the engine
treats the legacy value as a direct factory id when it is not one
of the recognised backend families.  For the v1.0.0 release, prefer
using the new `datachannel_backend` switch with the built-in factory
id `"web_transport"` (the production path).

#### 2.6.5 Framing mode (`datachannel_framing`)

A QUIC stream is a **byte** stream: `send()` boundaries are not
preserved.  `datachannel_framing` decides whether the payload carries
its own message boundary.

| Mode | Behaviour |
|---|---|
| `plugins::DataChannelFraming::kRaw` (**default**) | The payload reaches the transport verbatim.  Two `send()` calls may surface as one callback, one `send()` may surface as several, and the bytes carry **no channel identity**.  This is the mode Chrome's `RTCDataChannel`-over-WebTransport speaks, so it is the default and existing deployments keep working unchanged. |
| `plugins::DataChannelFraming::kFramed` | Every payload is wrapped in a length-prefixed frame that also carries the channel label.  One `send()` is one inbound message, and the label routes the message to the right channel. |

**Both ends must agree.**  The framing is a NimRTC-internal convention
(the plan's DEC-2 = (b)); there is no on-the-wire negotiation, because
Chrome will not speak one.  A framed peer cannot read a raw stream and
vice versa.

The field applies to every channel the engine creates.  For the frame
layout and the isolation rules see
[`docs/guides/datachannel.md`](../guides/datachannel.md).

#### 2.6.6 Channel priority (`datachannel_priority`)

```cpp
cfg.datachannel_priority = 16;   // lower value = higher priority
```

The value reaches the backend and orders the **data plane's own**
outbound queue: a high-priority control channel is not stuck behind a
bulk-transfer channel on the same QUIC connection.  The default is 128
(the value the engine used to hard-code before v1.2.0 — there was no
setter at all).

**What it does not do.**  The media plane and the data plane run
independent congestion control on two different connections, so the
scheduler cannot arbitrate between them.  The honest statement is: *the
data channel has its own connection and therefore its own priority; the
scheduler does not arbitrate between media and data* (the plan's
WP-E2).

#### 2.6.7 Partial-reliability budgets (`datachannel_max_retransmits` /
`datachannel_max_packet_lifetime_ms`)

```cpp
cfg.datachannel_framing = plugins::DataChannelFraming::kFramed;
cfg.datachannel_max_retransmits = 2;           // PR-count budget
cfg.datachannel_max_packet_lifetime_ms = 500;  // PR-TTL lifetime
```

Both fields reach every channel the engine creates and bound the
**application-level ARQ** that the partial-reliability modes run under
`kFramed` (plan WP-D4 / DEC-3 = (a)):

| Field | Meaning |
|---|---|
| `datachannel_max_retransmits` | Per-message retransmission budget. `0` = unlimited (the documented meaning in `plugins::DataChannelConfig`). |
| `datachannel_max_packet_lifetime_ms` | Per-message lifetime in milliseconds. `0` = unlimited. |

An expired or exhausted message is dropped and **the channel stays
open**. The ARQ is inactive in `kRaw` and in the reliable / unreliable
modes. See [`docs/guides/datachannel.md` §4](../guides/datachannel.md).

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
ICE transport binds. Both `0` = the OS assigns an ephemeral port.

**Leave them at 0.** The kernel's ephemeral allocation never returns a
port that the OS has reserved, which is exactly what a pinned range
cannot guarantee: Windows reserves whole blocks of UDP ports for the
hypervisor (Hyper-V / HNS), and a `bind()` inside a reserved block
fails with `WSAEACCES` (errno 10013). Those reservations move between
runs and between machines, so a pinned range passes on one CI runner
and fails on the next — the failure looks flaky and has nothing to do
with your code. libjuice walks the *entire* requested range before
giving up, so a single reserved port inside the range fails the whole
agent:

```
[warn]  udp.c:178: UDP socket binding failed on port range 127.0.0.1:[51000,51099], errno=10013
[error] conn_mux.c:270: UDP socket creation failed
```

Multiple `NimRTCEngine` instances on the same host do **not** need
disjoint ranges. The vendored libjuice in Mux mode hands every agent
its own OS-assigned port, so each engine gets its own socket and the
agents never see each other's STUN traffic.

On Windows you can inspect the current reservations with:

```powershell
netsh int ipv4 show excludedportrange protocol=udp
```

### 4.2 TURN

TURN servers are configured at runtime via
`engine.add_turn_server(host, port, user, pass)` — not in the
config struct, because TURN servers are typically per-session
(rotated by a relay-rotation / TURN service).

An overload takes an explicit `plugins::TurnTransport`
(`Udp` / `Tcp` / `Tls`) so the control connection can be forced over
TCP or TLS when UDP is blocked by the network. See
[engine_api.md](engine_api.md) §6.4.

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
