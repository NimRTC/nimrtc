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
│   ├── set_on_audio_frame(cb)        -- legacy PCM tap
│   ├── set_on_audio_frame_info(cb)   -- detailed PCM frame
│   ├── set_on_video_frame(cb)
│   ├── set_on_error(cb)
│   ├── set_on_state_change(cb)
│   ├── set_on_data_message(cb)
│   ├── set_on_data_message_ex(cb)    -- v1.2.0: adds the channel identity
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
│   ├── send_audio(const float* pcm, size_t n) → Status
│   └── remote_audio_format(AudioFormat* out) → bool
│
├── DTMF (RFC 4733, optional — off by default)
│   ├── send_dtmf(event_code, duration_ms) → bool
│   ├── set_on_dtmf(cb)
│   └── negotiated_dtmf_payload_type() → uint8_t
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
    └── Stats (video + audio counters) / stats()
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
| `on_audio_frame` | A decoded PCM frame is available (after SRTP decrypt + Opus decode + downmix + 3A post-process on the receive path) | `void(const float* pcm, size_t num_samples)` |
| `on_audio_frame_info` | The same decoded PCM frame, with its metadata (rate, channels, RTP timestamp, concealment / FEC flags, capture instant) | `void(const engine::AudioFrameInfo& info)` |
| `on_video_frame` | A decoded H.264 NAL is available on the receive path (pre-render) | `void(const uint8_t* nal, size_t len, bool keyframe)` |
| `on_error` | Any subsystem error fires (transport, DTLS, audio3A, codec, scheduler) | `void(uint32_t err, std::string_view msg)` |
| `on_state_change` | Engine state transitions (e.g. ICE → connected) | `void(const char* state)` |
| `on_data_message` | A DataChannel message arrives | `plugins::DataMessageCallback` |
| `on_data_state` | A DataChannel state transition (open / closed / error) | `plugins::DataChannelStateCallback` |
| `on_dtmf` | A completed DTMF key press arrives (RFC 4733; only when `EngineConfig::dtmf_enabled` is true) | `void(const plugins::DtmfEvent& event)` |

> **`on_audio_frame` is the legacy tap.**  Its `num_samples` argument is
> the **total number of interleaved float samples** —
> `num_samples_per_channel * channels` — and not the number of samples
> per channel.  The two are equal only for a mono stream.
>
> **`on_audio_frame_info` is the recommended surface.**  It carries the
> same PCM buffer plus the metadata a host needs to place the frame on
> a timeline.  When both callbacks are installed, the **legacy one fires
> first** and both receive the same buffer.  Installing one does not
> uninstall the other, and both may be absent.
>
> The `pcm` pointer of either callback points into a decoder-owned
> buffer that is reused for the next frame.  A host that needs to keep
> the audio must copy it.

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

int add_turn_server(std::string_view host, std::uint16_t port,
                    std::string_view username,
                    std::string_view password,
                    plugins::TurnTransport transport) noexcept;
```

Adds a TURN relay for candidate gathering. Requires `pre_open()`
first. Returns 0 on success; non-zero if the ICE transport is not
yet created.

The first overload reaches the server over plain TURN/UDP. The second
one selects the control-connection transport explicitly:

| `plugins::TurnTransport` | Meaning |
| --- | --- |
| `Udp` | TURN over UDP (default, RFC 5766) |
| `Tcp` | TURN over TCP, RFC 8656 §12 self-delimiting framing |
| `Tls` | TURN over TLS, same framing over a TLS stream |

`Tcp` is what enterprise networks need when UDP is blocked. `Tls`
adds transport security on top of the TURN channel; when the
underlying vendor build cannot negotiate TLS it falls back to plain
TCP.

The plugin-level seam exposes the same choice via
`plugins::IICETransport::add_turn_server_with_transport(...)` plus
`turn_transport_supported(TurnTransport)`, so a host application can
query whether a transport is usable before committing to it. The
plugin-layer entry point carries a distinct name rather than being an
overload of `add_turn_server()`: a second virtual with the same name
would be hidden by every existing `add_turn_server()` declaration in a
derived class, which breaks out-of-tree plugins that implement the
interface.

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

### 8.1 `send_audio()`

```cpp
uint32_t send_audio(const float* pcm_samples, size_t num_samples) noexcept;
```

Encode `pcm_samples` with the configured audio codec, RTP-packetise
it, SRTP-encrypt it, and submit to the scheduler. The buffer must
remain valid for the duration of the call (no copy made).

**The framing is the engine's business.**  `send_audio()` accepts any
block size: the PCM goes into a frame accumulator, and exactly one RTP
packet carrying exactly one frame of `EngineConfig::audio_ptime_ms`
milliseconds goes on the wire per frame.  A host that hands over one
whole frame at a time and a host that hands over seven samples at a
time put the *same* packets on the wire; only the moment of the call
differs.  A partial frame stays in the accumulator until the next call
completes it.

The media timestamp advances by the frame duration whether or not the
packet reached the wire, so a dropped frame leaves a gap in the
sequence number — never a hole in the timeline.

**Nothing fails silently.**  An empty buffer, a null buffer or an
encoder failure increments `Stats::audio_encode_errors` and fires
`on_error` (throttled to one report per second, so a broken stream
cannot flood the host).  The counter grows with *every* occurrence,
even the ones that are not reported.

Call cadence: `num_samples / (sample_rate_hz * channels)` matches
the frame duration in seconds; typical 10 ms / 20 ms Opus frames.
The engine handles framing internally as long as you keep the
cadence consistent — opportunistic re-pacing happens in `tick()`.

Returns `kOk` on success; non-zero on encode / encrypt failure
(surfaces via `on_error_`).

### 8.2 `set_on_audio_frame_info()`

```cpp
void set_on_audio_frame_info(AudioFrameInfoCallback cb) noexcept;
```

Installs the detailed audio-frame callback.  `AudioFrameInfo` carries
the decoded PCM plus its metadata:

| Field | Meaning |
|---|---|
| `pcm` | Interleaved PCM, float, `[-1.0, 1.0]`.  Valid only for the duration of the callback. |
| `num_samples_per_channel` | Samples per channel in `pcm`. |
| `sample_rate_hz` | The decoder's native rate (48000 for Opus).  Resample if the device rate differs. |
| `channels` | Channel count of `pcm`, after the downmix to `EngineConfig::pcm_channels`. |
| `rtp_timestamp` | RTP timestamp of the first sample in this frame. |
| `playout_time_us` | The engine-thread instant at which the frame was handed over (same epoch as `tick()`'s clock, **not** the Unix wall clock). |
| `capture_time_us` | The peer's capture instant, in Unix microseconds.  **0** when the cross-end timeline has not reconstructed it yet — the value is never faked from the local clock. |
| `concealed` | True when the frame is concealment (PLC) audio, i.e. the original packet never arrived. |
| `fec_recovered` | True when the frame was reconstructed from the peer's in-band FEC. |

Pass an empty function to remove the callback.  The legacy
`set_on_audio_frame()` stays, and when both are installed the legacy
one fires **first**, with the same buffer.

### 8.3 `remote_audio_format()`

```cpp
bool remote_audio_format(AudioFormat* out) const noexcept;
```

Reports the negotiated format of the inbound audio stream.  Returns
`false` when no audio track has been negotiated yet, in which case
`out` is left untouched.  `AudioFormat` carries
`sample_rate_hz`, `channels`, `payload_type` and `frame_duration_ms`.

A host that must know the rate before the first frame arrives — to
pre-allocate a resampler, say — asks the engine:

```cpp
nimrtc::engine::AudioFormat fmt{};
if (engine.remote_audio_format(&fmt)) {
    // fmt.sample_rate_hz, fmt.channels, fmt.payload_type,
    // fmt.frame_duration_ms are negotiated.
}
```

### 8.4 The receive path and the tick rate

The inbound audio is **decoded**: the host receives PCM, not the
compressed bit-stream.  The pipeline resolves its jitter buffer and
its decoder through the plugin registry, walks the media timeline on
the RTP timestamps, conceals the slots that never arrived, recovers a
lost frame from the peer's in-band FEC where possible, downmixes to the
configured output channel count and plays out **only** when the host's
`tick()` asks it to.

**The host must call `tick()` at ≥ 100 Hz for audio playout to be
smooth.**  A host that ticks at 30 Hz still gets continuous audio,
with a coarser granularity — the output cadence follows the media
time, not the arrival, so a slower tick delays the playout instead of
dropping frames.

There is **no fallback receiver**.  A missing decoder plugin or a
missing receive pipeline is a hard error (`kAudioDecoderMissing` /
`kAudioReceiverMissing`) that fires `on_error` exactly once and
disables the audio track; the inbound packets are then counted and
dropped, never decoded.

### 8.5 DTMF (RFC 4733)

DTMF is an **optional, in-band** capability: the digits travel as
`telephone-event` payloads on the audio m-line, not as a separate
stream and not out of band.

```cpp
bool send_dtmf(std::uint8_t event_code,
               std::uint16_t duration_ms) noexcept;
void set_on_dtmf(DtmfEventCallback cb) noexcept;
std::uint8_t negotiated_dtmf_payload_type() const noexcept;
```

**DTMF is off by default.**  `EngineConfig::dtmf_enabled` has to be
`true` before the engine offers a `telephone-event` payload type at
all; a default engine's offer carries no RFC 4733 line, and a peer
that sends one anyway is treated as an unknown payload type (see the
counting rules below).  `EngineConfig::dtmf_codec_name` selects the
codec plugin (default `"dtmf"`); an empty string disables the path
even when `dtmf_enabled` is true.

**The negotiation.**  The payload type is negotiated in the SDP
(RFC 4733 §2.3.2), never assumed:

* the offer carries `a=rtpmap:<pt> telephone-event/8000` and
  `a=fmtp:<pt> 0-16`, and the payload type joins the m-line's format
  list (RFC 3550 §5.14 scopes a payload type to its m-section);
* the answer carries the *negotiated* event codes — the peer's list
  narrowed to the codes this side implements — not a blind echo of the
  offer;
* `negotiated_dtmf_payload_type()` reports the number both ends use,
  or `0` when nothing was negotiated.  `send_dtmf()` fails in exactly
  that state, so a host can ask before it presses a key.

**Sending.**  `send_dtmf(event_code, duration_ms)` encodes one key
press.  RFC 4733 §2.5.1.2 describes the event as a *sequence*: the
packet is repeated while the key is held, each repeat carrying a
larger duration, and the packet that closes the event carries the
end-of-event bit.  The first packet goes out immediately; the repeats
are paced by `tick()`, so **the host must keep ticking for the event
to finish**.  The event packets share the audio stream's sequence
number space and timestamp base (RFC 4733 §2.1) and carry the marker
bit on the first packet only (RFC 4733 §2.2.2).

`duration_ms` is the length of the key press; `0` is rejected (RFC
4733 §2.3.5 makes a zero duration a *state*, not a key press), and
event codes above `16` are rejected (RFC 4733 §2.1 defines 0..16).

**Receiving.**  `set_on_dtmf()` installs the callback that receives the
completed events — `(event_code, duration_ms, end)`.  The engine
accumulates the duration across the repeats and reports the event
**once**, when the packet with the end bit arrives.

**The counting rules** (the boundary with the payload-type routing
fix): a `telephone-event` payload is *not* audio — it is a four-byte
event header, and handing it to the audio decoder would push the
header into an Opus decoder.  So:

| state                            | a received telephone-event packet |
|----------------------------------|------------------------------------|
| DTMF off, nothing negotiated     | counted as an unknown audio payload type and dropped |
| DTMF on, nothing negotiated      | counted as an unknown audio payload type and dropped |
| DTMF on, payload type negotiated | decoded and reported to the host |

In other words: a negotiated payload type is never an anomaly, and an
un-negotiated one always is.  The telephone-event plane has counters
of its own in `Stats` — `dtmf_packets_sent`,
`dtmf_packets_received`, `dtmf_packets_malformed`,
`dtmf_events_delivered` and `dtmf_payload_type` — because the audio
counters say nothing about it (the payload never reaches the audio
pipeline).

---

### 8.6 RTCP Extended Reports (RFC 3611)

The **Extended Report** (XR, RTCP payload type 207) is an optional
QoE / network-statistics channel.  NimRTC both *receives and decodes*
it and — when `EngineConfig::rtcp_xr_enabled` is set — *sends* it.

```cpp
plugins::IXrReport* xr_report() noexcept;          // non-owning
const plugins::IXrReport* xr_report() const noexcept;

// Outbound side (v1.2.0, session capability plan X2).
std::uint64_t xr_reports_sent_count() const noexcept;
```

The engine demultiplexes every inbound RTCP compound datagram and
routes the XR sub-packets into this consumer.  Read the decoded
report blocks out of its snapshot:

```cpp
if (auto* xr = engine.xr_report()) {
    const auto snap = xr->snapshot();
    if (snap.has_data) {
        for (const auto& blk : snap.last_packet.blocks) {
            switch (blk.type) {
                case plugins::XrBlockType::kReceiverReferenceTime:
                    // blk.receiver_reference_time.ntp_timestamp
                    break;
                case plugins::XrBlockType::kDlrr:
                    // blk.dlrr.sub_reports[]
                    break;
                case plugins::XrBlockType::kStatisticsSummary:
                    // blk.statistics_summary.lost_packets, .max_jitter, ...
                    break;
                default:
                    break;   // recognised but not parsed
            }
        }
    }
}
```

**The supported block types** are RFC 3611's own registry
(RFC 3611 section 6.2):

| BT | Block | NimRTC |
|----|-------|--------|
| 1 | Loss RLE | recognised, **not parsed** |
| 2 | Duplicate RLE | recognised, **not parsed** |
| 3 | Packet Receipt Times | recognised, **not parsed** |
| 4 | Receiver Reference Time | **parsed** |
| 5 | DLRR (Delay since Last Receiver Report) | **parsed** |
| 6 | Statistics Summary | **parsed** |
| 7 | VoIP Metrics | recognised, **not parsed** |

A block NimRTC does not parse is **skipped by its length**, never an
error — that is what makes the format extensible.

> **Note on block numbering.**  There is **no** block type named
> "Receiver Estimated Quality" in RFC 3611; that was the numbering of an
> early draft (where BT=1 was it and BT=6 was DLRR).  A report built
> with the draft's numbering would not interoperate with a conforming
> peer, so NimRTC only speaks the standard's actual numbering.

**Configuration.**  `EngineConfig::xr_report_name` selects the
consumer plugin (default `"nimrtc_xr"`, the built-in one).  An empty
string disables the consumer entirely — the XR sub-packet is then
recognised by the demultiplexer and dropped, which is the pre-v1.2.0
behaviour.

**A malformed report is counted, not fatal.**  `snapshot()` reports
`packets_malformed`; the read path never fails because one peer sent a
broken report.  `Stats::xr_report_blocks` counts the XR sub-packets
that were routed to the consumer — a peer that sends extended reports
and a counter stuck at zero means the routing is not wired.

#### 8.6.1 The outbound side

Setting `EngineConfig::rtcp_xr_enabled = true` makes the engine emit
one XR report per RTCP interval (one second), carrying:

| BT | Block | When it is sent |
|----|-------|-----------------|
| 4 | Receiver Reference Time | the peer negotiated `rcvr-rtt` |
| 5 | DLRR | the peer negotiated `rcvr-rtt` **and** a Receiver Reference Time report has been received |
| 6 | Statistics Summary | the peer negotiated `stat-summary` **and** inbound audio has been seen |

The DLRR's sub-block names the SSRC of the *peer* whose Receiver
Reference Time report is being answered, together with the middle 32
bits of that report's NTP timestamp and the delay since its arrival
(RFC 3611 section 4.5) — the three values a peer needs to compute the
round-trip time.
**The SDP negotiates it.**  The offer carries

```
a=rtcp-xr:rcvr-rtt=all stat-summary=loss,jitt
```

on the audio m-section, and the answer echoes the subset the engine
implements (RFC 3611 section 5.2).  RFC 3611 section 5.1 says a
participant "SHOULD NOT send XR blocks other than the ones indicated
by the parameters", so the engine sends nothing until the peer has
named the format — an XR report that was never negotiated is not
merely wasteful, it is a protocol violation.

**The field list is the peer's, not the engine's.**  RFC 3611 section
5.1 says the `stat-summary` parameter "contains a list indicating which
fields SHOULD be included in the Statistics Summary report blocks that
are sent" — so the L and J flag bits of the block the engine emits
follow the *peer's* list.  A peer that asked for `loss` alone gets a
block with the loss flag set and the jitter flag clear; a peer that
asked for `loss,jitt` gets both.  The engine produces loss and jitter
and nothing else, so a list naming only fields it does not measure
(`dup`, `TTL`, `HL`) narrows to nothing and the format is not echoed.

If the peer offers the attribute and the host does not want XR at all,
the answer carries the **bare** attribute (`a=rtcp-xr`, no
parameters): that is the RFC's way of saying "I understand the
attribute but send me no XR blocks".

---

### 8.7 TMMBR / TMMBN (RFC 5104)

The **Temporary Maximum Media Bit Rate** request is the receiver's
statement of the highest rate it can accept; the **TMMBN** is the
sender's confirmation of the bounding set that actually applies.

```cpp
// Outbound (v1.2.0, session capability plan X4 / X5).
std::uint32_t peer_tmmbr_limit_bps() const noexcept;  // the peer's limit
std::uint32_t last_tmmbr_bps() const noexcept;         // what we last reported
std::uint64_t tmmbr_sent_count() const noexcept;       // TMMBR + TMMBN sent
std::uint64_t tmmbr_received_count() const noexcept;    // TMMBRs honoured
```

**What the engine sends.**  With `EngineConfig::tmmbr_enabled = true`
the engine emits a TMMBR carrying the bit rate its bandwidth
estimator is *currently using* — never a stale number and never a
limit the engine does not honour itself.  A changed rate is reported
at the next RTCP interval; an unchanged rate is repeated only every
five intervals, because the peer may have missed the previous one
(RFC 5104 section 4.2.1.2).

The FCI entry names the *media source* the limit applies to (the
peer's inbound audio SSRC), and the "SSRC of media source" field of
the RTPFB header is zero — RFC 5104 section 4.2.1.2 says it is
unused.

**What the engine obeys.**  An inbound TMMBR is a limit the engine
must respect, and the engine does:

* the scheduler's session budget is clamped to
  `peer_tmmbr_limit_bps()`;
* the per-class pacing rates are clamped the same way;
* the **audio encoder** is clamped the same way — the encoder is what
  decides how many bits go on the wire, so a limit that stopped at the
  pacer would only *lose* frames rather than keep the promise;
* the limit **overrides `EngineConfig::audio_min_bitrate_bps`**: the
  local floor protects the audio against a congestion collapse, while
  the peer's limit is a commitment the engine made on the wire (the
  TMMBN is the acknowledgement of it), so the two are not of the same
  kind — the peer's limit wins, and the encoder may go *below* the
  local floor;
* the tightest limit wins — a looser TMMBR never raises a ceiling a
  tighter one has set (RFC 5104 section 3.5.4.2);
* a tuple naming a media source the engine does not send is recorded
  but **not** applied: it is somebody else's constraint.

The peer's limit is honoured even when
`EngineConfig::audio_bitrate_adaptive` is `false` — that switch says
"do not move the encoder because of the *estimate*", not "break the
promises the engine already made on the wire".

Every inbound TMMBR is answered with exactly one TMMBN (RFC 5104
section 4.2.2.2), even when the tuple was not accepted — that is what
lets a participant which missed the last TMMBR recover the state.

The TMMBN's FCI entry names the **owner** of the bounding tuple
(RFC 5104 section 4.2.2.1).  In the unicast case the receiver that
issued the TMMBR becomes the owner of the one bounding tuple as soon
as it has sent its first TMMBR (RFC 5104 section 4.2.2.2), so the
owner is the SSRC of the peer's feedback — the SSRC the TMMBR's own
"SSRC of packet sender" field carried.

**The SDP negotiates it.**  RFC 5104 section 7.2 gives TMMBR the
codec-control-message form:

```
a=rtcp-fb:<pt> ccm tmmbr
```

Only the joint subset of the offer's and the answer's CCM parameters
may be used, so the engine sends no TMMBR unless the peer's
description carried the `tmmbr` parameter *and* the host's own
configuration keeps the feature on.  The answer removes the `tmmbr`
parameter when the answerer does not support it — the rest of the
entry survives (RFC 5104 section 7.2 says the answerer removes the
*parameters* it does not support, not the whole entry).

`EngineConfig::disable_tmmbr` vetoes the feature in both directions:
no TMMBR is sent and the SDP carries no `tmmbr` parameter.  It
overrides `tmmbr_enabled`.

---

### 8.8 The RTCP support matrix

What the engine understands on the RTCP plane, so a host does not have
to guess.  "Send" means the engine *emits* the message; "Receive"
means it *parses* it.

| Packet | PT | Format | NimRTC |
|--------|----|--------|--------|
| SR | 200 | Sender Report | receive (timeline anchor) + **send** |
| RR | 201 | Receiver Report | receive (bandwidth collector) + **send** |
| SDES | 202 | Source Description | recognised, not parsed |
| BYE | 203 | Goodbye | recognised, not parsed |
| APP | 204 | Application-defined | recognised, not parsed |
| RTPFB | 205 | FMT 1 — NACK | receive |
| RTPFB | 205 | FMT 3 — **TMMBR** | **receive + send** |
| RTPFB | 205 | FMT 4 — **TMMBN** | **send** |
| RTPFB | 205 | FMT 15 — transport-cc | receive (ADR-018 bandwidth signal) |
| PSFB | 206 | FMT 1 — PLI | receive (video keyframe request) |
| PSFB | 206 | FMT 2 — SLI | receive (video keyframe request) |
| PSFB | 206 | FMT 15 — REMB | receive (bandwidth collector) |
| XR | 207 | BT 4 / 5 / 6 | **receive + send** (see §8.6) |

The XR block types NimRTC does *not* parse (Loss RLE BT=1, Duplicate
RLE BT=2, Packet Receipt Times BT=3, VoIP Metrics BT=7) are skipped
by their length field, never an error — see §8.6.

The codec-control-message family (RFC 5104) is **partially**
implemented: `tmmbr` is supported, `fir` / `tstr` / `tstn` / `vbcm`
are not.  An `a=rtcp-fb:` entry that names an unsupported CCM
parameter has that *parameter* removed from the answer — the rest of
the entry survives (RFC 5104 section 7.2).

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

#### GPU zero-copy (Phase 3 / C3)

By default the engine *stages* the frame into a CPU `I420` buffer (a
full-frame copy, plus a de-interleave for an NV12 source) before
handing it to the codec. A hardware capture source can avoid that copy
by carrying its native GPU handle in the frame:

```cpp
plugins::VideoSourceFrame f{};
f.gpu_backend     = plugins::GpuBackend::kWindowsD3D11Texture;
f.gpu_handle      = native_texture;   // borrowed for the call
f.gpu_pitch_bytes = 1920;
```

With `EngineConfig::video_zero_copy = true`, a frame that carries such a
handle is passed straight to `IVideoCodec::encode_zero_copy()` — no
staging, no copy. The path is taken only when **all three** hold:

1. `EngineConfig::video_zero_copy` is true,
2. the frame carries a live GPU handle, and
3. the codec reports `supports_zero_copy_encode()`.

Otherwise the CPU staging path runs unchanged, so turning the flag on
is always safe. `Stats::video_zero_copy_frames` and
`Stats::video_zero_copy_fallbacks` make the decision observable.

---

## 10. DataChannel control plane

```cpp
std::unique_ptr<plugins::IDataChannel>
create_data_channel(std::string_view label) noexcept;
```

Allocates a new `IDataChannel` from the resolved factory; opens it
with a default config (label, ordered, reliable, priority 128).
Returns `nullptr` if:

- the DataChannel backend is disabled (the resolved factory id is
  empty — see `docs/api/engine_config.md` §2.6.1);
- the engine is not `open()`ed yet;
- the factory id was not registered (caller forgot
  `core::register_all_default_plugins()`);
- the factory's `create()` returned null (catastrophic impl bug).

Must be called after `open()`. The returned channel is owned by the
caller; pass it to a transport-stack or use it directly via the
`IDataChannel` interface.

`set_on_data_message()` and `set_on_data_state()` install the
matching callbacks on **every** new channel created after the call.

### 10.1 `set_on_data_message_ex()` — the channel identity

```cpp
void set_on_data_message_ex(
    plugins::DataChannelMessageCallback cb) noexcept;
```

The classic `set_on_data_message()` callback carries the payload only,
so a host that opens two channels cannot tell which one a message
arrived on.  `set_on_data_message_ex()` is the **additive** counterpart
(v1.2.0, the plan's WP-C1): the plain callback keeps its exact
signature and behaviour — it is installed on every channel the engine
creates, as a fan-out shorthand.

```cpp
struct plugins::DataChannelMessage {
    std::string_view label;       // the channel's label
    std::uint32_t    channel_id;  // engine-assigned id (1-based, per engine)
    plugins::BufferView data;     // the payload (same bytes the plain callback gets)
};
```

The engine assigns `channel_id` (monotonically increasing per engine,
starting at 1) and fills `label` when the backend does not report one.

With the framed mode (`EngineConfig::datachannel_framing =
plugins::DataChannelFraming::kFramed`) the *peer's* label decides which
channel a message belongs to.  With the raw mode there is no identity
on the wire, so every channel of the connection sees the same bytes —
use the framed mode (or one channel per connection) if you need
isolation.  See [`docs/guides/datachannel.md`](../guides/datachannel.md).

> **Status**: the default backend is `"web_transport"` (WebTransport /
> QUIC), which is the only backend with a wire path. The `"sctp"` backend
> was **retired from the public selectors in v1.2.0** — it was an
> in-process loopback socket that could never reach a peer. Physical
> SCTP-over-DTLS plumbing was never landed and is not part of the
> current architecture — see `docs/adr/ADR-014-web-transport-vendor.md`.

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
| `xr_report()` | `IXrReport*` | Reading the peer's decoded RFC 3611 extended reports. |
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
    // ---- The reading's identity (v1.2.0, plan Track S / S1) ------------
    // When the rest of this struct was measured.  `snapshot_time_us`
    // is on the engine's monotonic clock (the same origin `tick()`'s
    // `now_us` uses); `snapshot_seq` increases by one per reading and
    // therefore orders two readings even when their timestamps are
    // identical.
    int64_t  snapshot_time_us = 0;
    uint64_t snapshot_seq     = 0;

    int srtp_drops = 0;

    // Video counters (v1.1 Track V) — cumulative since open().
    uint64_t video_frames_sent = 0;
    uint64_t video_packets_sent = 0;
    uint64_t video_bytes_sent = 0;
    uint64_t video_frames_received = 0;
    uint64_t video_nack_seqs_queued = 0;
    uint64_t video_keyframes_q = 0;
    uint64_t video_feedback_applied = 0;
    uint64_t video_keyframes_applied = 0;

    // Video zero-copy counters (Phase 3 / C3) — cumulative since open().
    uint64_t video_zero_copy_frames = 0;     // frames that took the GPU path
    uint64_t video_zero_copy_fallbacks = 0;  // GPU frames that fell back to CPU

    // Audio send counters (audio pipeline WP-5) — cumulative since open().
    uint64_t audio_frames_sent = 0;
    uint64_t audio_bytes_sent = 0;
    uint64_t audio_encode_errors = 0;
    uint64_t audio_samples_in = 0;
    uint64_t audio_samples_pending = 0;

    // Audio receive counters (audio pipeline WP-8 / spec B-13).
    uint64_t audio_packets_sent = 0;
    uint64_t audio_frames_received = 0;
    uint64_t audio_samples_received = 0;
    uint64_t audio_concealed_frames = 0;
    uint64_t audio_fec_recovered_frames = 0;
    uint64_t audio_packets_lost = 0;
    uint64_t audio_decode_errors = 0;
    uint32_t audio_jitter_ms = 0;
    uint32_t audio_playout_delay_ms = 0;

    // Adaptive audio bitrate — instantaneous, not cumulative.
    uint32_t audio_encoder_bitrate_bps = 0;

    // Network feedback / BWE loop (v1.2.0 M1) — mixed semantics.
    uint64_t net_feedback_blocks    = 0;  // RTCP blocks routed to the collector
    uint64_t bwe_feedback_samples  = 0;  // samples handed to IBwe::on_feedback()
    uint32_t bandwidth_estimate_bps = 0; // current BWE target, in bit/s

    // RTCP Extended Reports (v1.2.0, session plan X1).
    uint64_t xr_report_blocks      = 0;  // XR blocks routed to the consumer

    // The data channels the host opened (v1.2.0, plan Track S / S6).
    // One entry per channel, in creation order; empty when the host
    // never opened one.  See §13.1 for the entry's fields.
    std::vector<DataChannelStats> data_channels;
};
Stats stats() const noexcept;

// ---- The named, snapshot-able view (v1.2.0, plan Track S / S1) --------
//
// `stats_snapshot()` returns the same numbers as `stats()`, but keyed
// by name, stamped with the reading's instant and sequence number.
// `stats_delta_since()` diffs the current reading against an earlier
// one and answers with the *increment* over the interval.
//
// The types live in `nimrtc/core/stats_registry.hpp`
// (`core::StatSnapshot`, `core::StatsDelta`).
core::StatSnapshot stats_snapshot() const noexcept;
core::StatsDelta   stats_delta_since(const core::StatSnapshot& previous) const noexcept;
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

The remaining fields are **cumulative counters since `open()`**, except
`audio_encoder_bitrate_bps` and `bandwidth_estimate_bps`, which are the
**instantaneous** values (the outbound audio encoder's current bitrate and
the bandwidth estimator's current target respectively). `audio_jitter_ms`
and `audio_playout_delay_ms` are likewise instantaneous measurements
rather than running totals.

### 13.1 The snapshot lifecycle (v1.2.0, plan Track S / S1)

The flat struct answers *"how much has happened since `open()`"* and
nothing else.  A host that wants a **rate** — bytes per second, packets
per second — has to take two readings and work out how far apart they
were, and it has to know which fields are counters and which are gauges.
The snapshot lifecycle gives it both.

```cpp
auto base = engine.stats_snapshot();       // the baseline reading
// ... the session runs ...
auto d = engine.stats_delta_since(base);    // the increment over the interval

if (d.valid()) {
    std::int64_t frames = 0;
    if (d.counter_delta("", "video_frames_sent", frames)) {
        // `frames` is the increment that actually happened between the
        // two readings -- not the absolute value.
        const auto seconds = d.duration_us() / 1e6;
        const auto fps = seconds > 0.0 ? frames / seconds : 0.0;
    }

    std::int64_t bitrate = 0;
    if (d.gauge_value("", "video_tx_bitrate_bps", bitrate)) {
        // A gauge has no increment: the difference of two instantaneous
        // bitrates is not a bitrate.  The value at the end of the
        // interval is the answer.
    }
}
```

The contract:

- **`counter_delta()` refuses to answer for a gauge** and
  **`gauge_value()` refuses to answer for a counter.**  Getting the
  classification wrong is the classic statistics bug (subtracting two
  EMA bitrates and reporting a negative rate), so the semantics travel
  with the number and the API refuses to mix them up.
- **`duration_us()`** is the length of the interval, so a host can turn
  an increment into a rate without keeping its own clock.
- **A default-constructed snapshot has no usable baseline.**  The delta
  would be measured against a made-up zero, so `valid()` is false.
- **The sequence number orders two readings even when their timestamps
  are identical** (a coarse clock, or two readings inside the same
  microsecond), so "the interval between these two readings" is always
  well-defined.
- **A snapshot outlives the engine.**  It owns its data, so a host may
  keep it as a baseline after the engine is destroyed.

The registry is a **projection** of the flat struct, taken at reading
time: the flat struct stays the single authority, so the two views can
never drift apart.  The registry is an *instance*, not a singleton — two
engines in one process get two registries.

**This is not a standard `RTCStatsReport`.** The struct is flat and
engine-wide, with one exception: `inbound_rtp_streams` carries a
per-source breakdown (see §13.2). There is still no ICE
candidate-pair / RTT dimension. The snapshot lifecycle adds the
*time* and the *semantics*, not the missing dimensions. The
`src/modules/video_stats/` module **is** wired into the engine (the
private bridge `src/engine/src/video_stats_bridge.hpp`), so its freeze
counts, keyframe intervals, QP samples and drop-cause breakdown are
reachable through this accessor as `video_freeze_count` /
`video_freeze_total_ms` / `video_keyframe_interval_avg_ms` /
`video_qp_sum` / `video_qp_count` / `video_frames_dropped_jb` /
`video_frames_dropped_decoder`. For the full picture — the planned
stream-level shape and the missing dimensions — see
`docs/plan/stats-surface-plan.md` (landed later in the v1.2 stack, so
referenced here by path, not by link).

### 13.2 The inbound per-source breakdown (v1.2.0, plan Track S / S2)

The flat counters answer *"how much arrived"*, but not *"from
whom"*.  A peer that sends **simulcast** (RFC 5956 — several encodings
of the same picture on different SSRCs) puts three different
qualities into one counter, and a host that receives from more than
one source has the same problem.  There is no way to ask *"how many
packets arrived on SSRC X?"* — which is the question a loss
investigation actually asks.

`Stats::inbound_rtp_streams` is the receive-side counterpart of
`Stats::video_streams`: one entry per inbound RTP source.

```cpp
struct Stats {
    // ---- The inbound per-source breakdown (v1.2.0, plan Track S / S2) ----
    // One entry per inbound RTP source, keyed by SSRC.
    std::vector<InboundRtpStats> inbound_rtp_streams;
};

struct InboundRtpStats {
    std::uint32_t ssrc = 0;            // the RTP source identifier
    std::uint8_t  payload_type = 0;    // observed, not negotiated
    std::string   kind;                // "video", "audio" or "unknown"

    std::uint64_t packets_received = 0;
    std::uint64_t bytes_received = 0;
    std::uint64_t packets_lost = 0;      // the gaps in the sequence space
    std::uint64_t nacks_sent = 0;       // the retransmission requests sent
    std::uint64_t frames_received = 0;   // the assembled access units
    std::uint64_t frames_incomplete = 0; // arrived but not assemblable
    std::uint32_t jitter_ms = 0;
    std::uint32_t playout_delay_ms = 0;
};
```

The contract:

- **The SSRC is the key**, because the SSRC is the only thing on the
  wire that identifies a source.  The payload type says how to
  *decode* a packet, not who sent it.
- **The entry accumulates over the life of the source.**  A source
  that restarts (a new SSRC) gets a new entry; a source that
  disappears keeps its entry, so a host reading the list after the
  peer stopped still sees the totals it produced.
- **`packets_lost` is the loss figure**, counted by the receive
  pipeline — the only party that sees the sequence space.
- **The payload type is observed, not negotiated.**  A peer that
  changes its payload type mid-stream (RFC 3264 §5.1 allows a PT
  change after a re-INVITE) shows up as a changed value.

The outbound side keeps its existing `Stats::video_streams`
(`VideoStreamStats`: `ssrc` / `packets_sent` / `bytes_sent` /
`keyframes_sent`); this row adds the missing inbound counterpart.

### 13.3 The per-stream entries (v1.2.0, plan Track S / S2 and S4)

The flat counters above are **engine-wide**.  Two lists add the missing
*per-stream* dimension, and both are **additive** — the flat fields stay,
and they describe the same stream the list's first entry carries.

```cpp
// The video side (remote-desktop plan H4): one entry per active video
// stream, the default one included.
std::vector<VideoStreamStats> video_streams;

// The audio side (plan Track S / S4): one entry per active audio track.
std::vector<AudioStreamStats> audio_streams;
```

#### `VideoStreamStats`

| Field | Meaning |
|---|---|
| `stream_id` | The stable stream id (`"default"`, `"screen"`, `"camera"`, …). |
| `ssrc`, `payload_type`, `mid` | The stream's identity on the wire. |
| `codec_name`, `backend_id` | The encoding and the backend it runs on. |
| `bitrate_bps`, `keyframe_rate_hz` | The stream's current rates. |
| `ladder_level`, `bandwidth_share_bps`, `bandwidth_weight` | The rate controller's rung and the stream's share of the session budget. |
| `frames_sent`, `frames_received`, `frames_encoded` | The frame counts. |
| `packets_sent`, `bytes_sent`, `keyframes_sent` | The outbound RTP counts. |

#### `AudioStreamStats`

One **audio track** — the session's audio m-line — in *both* directions.

| Field | Meaning |
|---|---|
| `stream_id` | The stable track id (`"default"` for the engine's own track). |
| `ssrc` | The SSRC of the **outbound** encoding. |
| `inbound_ssrc` | The SSRC of the **inbound** source (0 when nothing arrived). The peer picks its own SSRC, so the two ends of the track differ. |
| `payload_type`, `codec_name` | The negotiated payload type and the encoding name. |
| `clock_rate_hz`, `channels`, `frame_duration_ms` | The stream's format. |
| `bitrate_bps`, `encoder_bitrate_bps` | The current outbound rate, and the encoder's current rate (the BWE-linked adaptation lowers it). |
| `packets_sent`, `bytes_sent` | The outbound RTP counts. |
| `encode_errors`, `samples_in` | The PCM the host handed over, and the frames that could not be encoded. |
| `packets_received`, `bytes_received` | The inbound RTP counts (the whole packet on the wire). |
| `frames_received`, `samples_received` | What the engine handed to the host. |
| `frames_concealed`, `frames_fec_recovered` | The frames produced by concealment and by in-band FEC. |
| `packets_lost`, `decode_errors` | The pipeline's own figures — it is the only party that sees the source's sequence space. |
| `jitter_ms`, `playout_delay_ms` | The pipeline's instantaneous measurements. |

**The audio list is not a second per-SSRC table.**  The per-SSRC inbound
breakdown lives in `Stats::inbound_rtp_streams` (plan Track S / **S2**),
which carries every inbound source — audio included — keyed by SSRC.  The
audio list adds the part S2 cannot know: the **outbound** half of the
track, and the *pipeline's* figures for the source the receive side is
actually working on.  A host that summed both lists would otherwise count
every inbound packet twice.

**The audio plane is single-stream today**, so the list normally holds
exactly one entry.  It is a **list** anyway: a second audio source (a
second microphone, a programme bus) would make the flat counters
unattributable, and a list grows where a single struct would have to be
redrawn — redrawing it would break every caller at once.

### 13.4 The data-channel statistics (v1.2.0, plan Track S / S6)

`Stats::data_channels` carries **one entry per data channel the host
opened** with `create_data_channel()`, in creation order.  The list is
empty when the host never opened a channel, so a host that does not use
the data plane sees no change in its telemetry.

```cpp
struct DataChannelStats {
    uint32_t channel_id = 0;   // the engine-assigned id (1-based)
    std::string label;         // the label the host gave the channel
    std::string reliability;    // "reliable-ordered" / "unreliable" /
                                // "partial-reliable-ttl" /
                                // "partial-reliable-count"

    uint64_t messages_sent = 0;          // the backend's count
    uint64_t bytes_sent = 0;             // the backend's count
    uint64_t messages_received = 0;      // the engine's count
    uint64_t bytes_received = 0;         // the engine's count
    uint64_t messages_dropped = 0;       // the backend's count
    uint64_t messages_retransmitted = 0; // the backend's count
    uint64_t messages_pending = 0;       // the backend's gauge

    std::string state;   // "connecting" / "open" / "closing" /
                         // "closed" / "error" ("" when none was reported)
};
```

**The two-sided model.**  The counters come from two different parties,
and that is the whole design:

- The **inbound** half (`messages_received` / `bytes_received`) is the
  *engine's* account: the engine installs the channel's callbacks, so it
  sees every inbound message and counts it.
- The **outbound** half (`messages_sent` / `bytes_sent` /
  `messages_dropped` / `messages_retransmitted` / `messages_pending`) is
  the *backend's* account, reached through the additive
  `plugins::IDataChannel::stats()` seam.  The engine cannot derive
  those numbers — it only sees the `send()` call, and what happens to
  the message afterwards (queued, retransmitted, dropped when its
  lifetime expired) happens inside the backend.

A backend that does not implement the seam reports zeroes there.  The
engine republishes those zeroes rather than inventing a number.

**The counters are cumulative** since the channel was created, so the
difference of two readings is the increment.  `messages_pending` is the
exception: it is a *gauge* — the depth of the outbound queue at the
moment of the reading.

**`reliability` is a name, not an enum.**  The engine publishes the mode
as the canonical string so a host does not have to know the enum's
numeric values.  A host that wants to know *why* a message was dropped
reads this field: the two partial modes are the only ones that drop.

### 13.5 The transport plane (v1.2.0, plan Track S / S5)

The statistics surface above describes the *media*.  `Stats::transport`
describes the **path the media travels over**: ICE (RFC 8445) → DTLS
(RFC 6347) → SRTP (RFC 3711).

Before S5 the transport had no statistics of its own: `Stats::srtp_drops`
counted the packets the outbound path could not encrypt, `dtls_state()`
exposed the DTLS state, and ICE — the plane that decides *where the
packets go* — had **nothing at all**: no candidate pair, no round-trip
time, no restart count.

```cpp
struct Stats {
    // ---- The transport plane (v1.2.0, plan Track S / S5) ----
    TransportStats transport;
};
```

#### `TransportStats`

**ICE (RFC 8445)**

| Field | Meaning |
|---|---|
| `ice_state` | The agent's state: `"disconnected"`, `"gathering"`, `"connecting"`, `"connected"`, `"completed"` or `"failed"`.  Empty before the engine has a transport.  The same vocabulary as `ice_state_string()`. |
| `ice_restart_count` | The ICE restarts performed so far (RFC 8445 §9.1.1.1).  0 means the session never restarted. |
| `selected_pair` | The candidate pair the agent nominated, or empty.  Empty until the connectivity checks settle on a path, which is *after* the agent reaches Connected. |
| `relayed` | True when the selected pair goes through a TURN relay.  *Derived* from the pair's candidate types, published separately because it is the one bit a host actually branches on. |
| `rtt_ms` | The round-trip time in milliseconds, **instantaneous** (the latest measurement, not a running total).  0 until a Receiver Report with a usable LSR / DLSR pair has arrived. |
| `has_rtt` | True once a round-trip time has actually been *measured*.  This is what distinguishes "the RTT is 0 because the path is fast" from "the RTT is 0 because nothing was ever measured". |

**DTLS (RFC 6347)**

| Field | Meaning |
|---|---|
| `dtls_state` | The handshake's state: `"closed"`, `"initial"`, `"hello-verify"`, `"hello-sent"`, `"hello-received"`, `"certificate-received"`, `"key-exchange"`, `"change-cipher-spec"`, `"finished"`, `"connected"` or `"failed"`. |
| `dtls_role` | `"client"` or `"server"` — the role the negotiation resolved (RFC 5763 §5).  Empty before the negotiation has run: "the role is not negotiated yet" is a different fact from "we are the server". |
| `dtls_fingerprint` | The local certificate's fingerprint, in the colon-separated UPPER hex form of RFC 8122 (`"AA:BB:..."`). |

**SRTP (RFC 3711)**

| Field | Meaning |
|---|---|
| `srtp_installed` | True once the DTLS-derived keying material has been installed.  This is the *authority* on "is this session encrypted". |
| `srtp_rtp_packets_protected` / `_unprotected` | The RTP packets the SRTP layer encrypted / decrypted. |
| `srtp_rtcp_packets_protected` / `_unprotected` | The RTCP packets the SRTP layer protected / unprotected. |
| `srtp_auth_failures` | The packets the SRTP layer refused: an authentication-tag mismatch, a replay, or a malformed packet.  A non-zero count means the peer sent something the engine could not authenticate — the most security-relevant number on this surface. |
| `srtp_drops` | The packets the engine had to send but could not encrypt.  The same figure the flat `Stats::srtp_drops` carries, repeated so a host that reads only the transport entry has the complete picture. |

#### `IceCandidatePairInfo`

```cpp
struct IceCandidatePairInfo {
    IceCandidateInfo local;   // our end of the pair
    IceCandidateInfo remote;  // the peer's end of the pair
};

struct IceCandidateInfo {
    std::string   address;   // the IP literal (no brackets, even for IPv6)
    std::uint16_t port = 0;
    std::string   protocol;  // "udp" / "tcp", lower-cased
    std::string   type;      // "host" / "srflx" / "prflx" / "relay"
};
```

Both ends are published because a diagnosis usually needs both — *"where
does my traffic leave from, and where does it arrive?"* — and because the
two ends can differ in **kind**: a local host candidate reaching a remote
relay candidate is the shape of a TURN-relayed path.

#### What this surface deliberately does *not* do

**No new probes.**  The RTT is the one the engine already measures from
the peer's Receiver Reports (the LSR / DLSR pair against our own Sender
Report, RFC 3550 §6.4.1) — the *same* figure the bandwidth estimator
consumes.  The statistics surface republishes it; it does not measure it
a second time, because two measurements of one quantity would eventually
disagree.

**No per-candidate-pair list.**  libjuice nominates exactly one pair and
does not expose the others it tried, so a list of candidate pairs would
be a list of one — and would suggest the engine can enumerate paths it
cannot actually see.  What the engine publishes is the pair that is *in
use*, plus the counters that say whether it is working.

**No copies of another owner's state.**  The ICE state and the candidate
pair are owned by the ICE agent (read through the `plugins::IICETransport`
seam), the DTLS state by the DTLS session, the SRTP counters by the SRTP
context, and the round-trip time by the network-feedback collector.  The
surface republishes each of them; it keeps no second copy that could go
stale or disagree.

### 13.6 The remote-desktop pipeline breakdown (v1.2.0, plan Track S / S7)

The QoE block (`Stats::qoe`) measures what the operator **felt** — freezes,
latency, sharpness.  `Stats::remote_desktop` answers the operator's
follow-up question: **why**.  It is the video pipeline broken into its
stages, each reported in the unit a human reasons about.

A screen-share session has three stages that matter, and each one fails
in its own way: the **capture** is not grabbing the desktop fast enough,
the **encoder** cannot keep up with the capture cadence, or the **decoder**
on the peer cannot keep up with what we send.  Before S7 the answers were
scattered across a dozen prefixes (`video_stage_encode.mean_us`,
`video_capture_frames`, `video_frames_decoded`, …) and the host had to
know which prefix carried which half — and still could not answer the
question, because there was **no capture frame rate** and **no
decode-side cost** anywhere on the surface.

```cpp
struct Stats {
    // ---- The remote-desktop breakdown (v1.2.0, plan Track S / S7) ----
    RemoteDesktopStats remote_desktop;
};
```

#### `RemoteDesktopStats`

| Field | Meaning |
|---|---|
| `capture_fps` | The rate the capture device delivered frames, in frames per second.  Measured on the **device's own capture timestamps** (`(frames - 1) / (last_ts - first_ts)`), so a device that stamps frames itself still yields the rate it actually ran at.  0 when fewer than two frames were seen — "not measurable yet", not "zero frames per second".  0 also when the device's clock **moved backwards** during the session (a re-created source, a re-negotiated portal): the span then no longer describes the interval the count covers, so the rate is reported as unknown rather than as a plausible-looking wrong number. |
| `capture_ms` | The mean cost of the capture device handoff, in milliseconds. |
| `encode_fps` | The rate the encoder ran at, in frames per second — the stage's *throughput*, over the stage's whole life. |
| `encode_ms` | The mean cost of the codec's `encode()` call, in milliseconds. |
| `encode_p95_ms` | The p95 of the encode cost, in milliseconds — **the number a stutter shows up in**.  A good mean with a terrible p95 is a session that hiccups. |
| `decode_fps` | The rate the decoder ran at, in frames per second. |
| `decode_ms` | The mean cost of the codec's `decode()` call, in milliseconds. |
| `decode_p95_ms` | The p95 of the decode cost, in milliseconds. |
| `frames_captured` | The frames the capture device delivered.  A delivery is identified by its **sequence number**, not its timestamp: a bounded device queue drops the oldest frame when the pump falls behind, and such a frame's successor may carry a timestamp that is not greater than the last one the pump saw.  Those frames are still deliveries and are counted; only a repeated sequence number (the same frame handed over twice) is not. |
| `frames_encoded` | The frames the encoder processed. |
| `frames_decoded` | The frames the decoder processed. |
| `frames_dropped_before_encode` | `max(0, frames_captured - frames_encoded)` — the frames that were captured but never reached the encoder. |
| `frames_decoded_without_local_encode` | `max(0, frames_decoded - frames_encoded)` — **informational only**, not an error counter.  A receive-only session has an empty encode stage and a busy decode stage, so this is deliberately kept out of the drop accounting. |

#### Everything here is *derived*, never measured twice

Each value is read from a measurement that **already exists**:

- The **costs** come from the per-stage instrumentation
  (`Stats::video_stage_capture` / `video_stage_encode` /
  `video_stage_decode`) — the same table the flat fields expose.  The
  breakdown converts its units (nanoseconds → milliseconds) and adds
  nothing, so the two views **cannot drift apart**.
- The **rates** come from two sources that the stage table does not
  carry: the capture rate from the device's own capture timestamps, and
  the encode / decode rates from each stage's wall-clock span (the time
  between its first and its latest sample).

That is why adding this block cost **no work on the frame path**: the
stage instrumentation already bracketed every stage, and the capture
timestamps already travelled with every frame.

#### The zero reading

Every field is 0 until the thing it describes has actually happened.  A
host that does not configure a capture device has no capture stage, and a
session that never received a video frame has no decode stage — the
zeros are the **correct** reading, not a missing value.

### 13.7 The GPU delivery path (v1.2.0, plan Track E / E5)

`VideoSinkStats` (the sink-side counters, not the engine `Stats`) carries
three counters that say **which route a decoded frame took to the
display**:

| Field | Meaning |
|---|---|
| `gpu_device_copies` | GPU surfaces copied **device-to-device** into the present target.  The picture never entered host memory between decode and present — this is the zero-copy hand-off, confirmed. |
| `gpu_staging_fallbacks` | GPU surfaces that could not go device-to-device and had to be **staged through host memory**.  The D3D11 sink counts here when a surface is not BGRA8: it presents from the surface's CPU mirror if there is one, and otherwise refuses it so the engine decodes to host pixels. |
| `gpu_surfaces_refused` | Surfaces refused outright: a foreign backend (a Metal IOSurface on Windows), a null handle, or a zero-size surface.  A non-zero value here is a **configuration error**, not a fallback. |

The split is the point.  On a healthy DXGI loop the first counter grows
and the other two stay at zero.  A host that sees
`gpu_staging_fallbacks` move is looking at a decoder or driver
configuration that is not zero-copy **all the way to the display** — the
capture → encode leg may still be zero-copy while the decoded NV12
surface is converted on the CPU.  The engine's render-path rule
(`video_render_path.hpp`) decides the *route* (GPU surface vs CPU
staging) once at `open()`; these counters describe what actually
happened per frame afterwards.

The GPU copy itself is performed on the sink's **presenter thread**, not
on the caller's: a GPU frame handed to `render(const GpuBuffer&)` costs
the producer a reference-count bump, and the device-to-device copy
happens on the presentation clock, exactly like a CPU frame.  That is
what keeps a sink that receives a mix of GPU and CPU frames from
presenting twice per tick or skewing `frames_dropped_present`.

---

### 13.8 The stable field set (v1.2.0, plan Track S / S8)

`Stats` grew one additive block at a time (S1 – S7 above).  This section
is where that growth is written down as a **contract**: every field
`Stats` carries today is listed below, with its type and the definition
the header gives it.  The table is **generated from the header** by
`tools/dump_stats_fields.py --markdown`, and a CI gate
(`--check`) fails when the table and the header disagree — so it cannot
go stale, and a field added to the header is visible here the same day.

#### What "stable field set" promises

- **It stays.**  A field is not renamed, retyped, or removed in a minor
  release.  If a measurement is wrong, it is fixed in place.
- **Its meaning does not change.**  `capture_ms` is the mean cost of the
  device handoff; it is not quietly redefined later.  When a field's
  *scope* needs to grow (the S7 `capture_fps` fix), it grows in the way
  the field always meant — not by reinterpreting the number.
- **It only grows.**  A new block is a *new* field (or a nested type
  such as `RemoteDesktopStats`), never a reuse of an old one — which is
  why S1, S2, S4, S5, S6 and S7 all added members.

The promise is about the **field set**, not the C++ layout: `Stats` is
not an ABI type, and a host reads it through the header it compiled
against.  The layout and the exported symbols are
`scripts/check_api_manifest.py`'s business; this table is the field
set's.

#### The sample apps read the same fields

The sample apps are the field set's live check — they compile against
`Stats` and print what they read, so a field that is broken *in
practice* shows up in their output, not only in a unit test.

| App | What it prints from `Stats` |
|---|---|
| `examples/demo-p2p` | `audio-rx`: `audio_frames_received` / `audio_samples_received` / `audio_concealed_frames` / `audio_fec_recovered_frames` / `audio_packets_lost` / `audio_decode_errors` / `audio_jitter_ms` / `audio_playout_delay_ms`.  The `audio-rx summary` line is parsed by `tools/run_e2e_acceptance.py`, so its shape is an e2e-contract too. |
| `examples/rd_device` | the capture half: `video_capture_device_frames_captured` / `video_capture_pumped_frames` |
| `examples/rd_viewer` | the receive half: `video_frames_decoded` / `video_frames_rendered` / `video_frames_dropped_decoder` |
| `examples/rd_verify` | the remote-desktop pipeline row by row: capture, zero-copy, encode / decode backend and hardware flag, frames sent / received / decoded / rendered, and the pass / fail verdict |

#### The nested entries

The fields whose type is a struct or a list carry one entry per
*thing*, not one number for the whole session.  Their shapes:

**`VideoStreamStats`** — one per active video stream (the default stream
included): identity (`stream_id`, `ssrc`, `payload_type`, `mid`,
`codec_name`, `backend_id`), the live rate state (`bitrate_bps`,
`ladder_level`, `bandwidth_share_bps`, `bandwidth_weight`,
`keyframe_rate_hz`) and the counters (`frames_sent`,
`frames_received`, `frames_encoded`, `packets_sent`, `bytes_sent`,
`keyframes_sent`).  The flat `video_*` fields describe the default
stream, so a single-stream host reads either.

**`InboundRtpStats`** — one per inbound RTP source, keyed by SSRC and
accumulated for the life of the source: `ssrc`, `payload_type` (as
*observed* on the wire), `kind` (`"video"` / `"audio"` /
`"unknown"`), `packets_received`, `bytes_received`, `packets_lost`,
`nacks_sent`, `frames_received`, `frames_incomplete`, `jitter_ms`.
A source that restarts gets a new entry; one that disappears keeps
its totals.

**`AudioStreamStats`** — one per active audio stream: the outbound
identity (`ssrc`, `payload_type`, `codec_name`, `clock_rate_hz`,
`channels`, `frame_duration_ms`, `bitrate_bps`, `encoder_bitrate_bps`),
the send counters (`packets_sent`, `bytes_sent`, `encode_errors`,
`samples_in`) and the receive half (`inbound_ssrc`,
`packets_received`, `bytes_received`, `frames_received`,
`samples_received`, `frames_concealed`, `frames_fec_recovered`,
`packets_lost`, `decode_errors`, `jitter_ms`, `playout_delay_ms`).

**`DataChannelStats`** — one per channel opened with
`create_data_channel()`: `channel_id`, `label`, `reliability`, the
backend's outbound counts (`messages_sent`, `bytes_sent`,
`messages_dropped`, `messages_retransmitted`, `messages_pending`), the
engine's inbound counts (`messages_received`, `bytes_received`) and the
last reported `state`.

**`TransportStats`** — the session's path, three planes deep: **ICE**
(`ice_state`, `ice_restart_count`, `selected_pair`, `relayed`,
`rtt_ms` + `has_rtt`), **DTLS** (`dtls_state`, `dtls_role`,
`dtls_fingerprint`) and **SRTP** (`srtp_installed`, the protected /
unprotected RTP + RTCP counters, `srtp_auth_failures`, `srtp_drops`).
`selected_pair` (`IceCandidatePairInfo`) names both ends' `address` /
`port` / `protocol` / candidate `type`.

**`VideoSyncStats`** — the render clock's decisions: `frames_presented`,
`frames_dropped_sync` (the video was ahead of the audio),
`frames_delayed` (it was behind), `total_delay_us` and
`last_skew_ms` (positive = video behind).

**`RemoteDesktopStats`** — the pipeline breakdown, documented in §13.6.

#### The field-set table

The authoritative copy lives in a generated file —
[`stats_field_reference.md`](./stats_field_reference.md) — and the CI
gate compares *that* file with the header:

```
python tools/dump_stats_fields.py --markdown > docs/api/stats_field_reference.md
python tools/dump_stats_fields.py --check    docs/api/stats_field_reference.md
```

The same table is reproduced here so this document stands alone.  A
field whose meaning column is `--` has no per-field comment in the
header; its section note above it is the definition.

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `snapshot_time_us` | `std::int64_t` | The reading's timestamp, in microseconds on the engine's monotonic clock (the `tick()` timebase). |
| 2 | `snapshot_seq` | `std::uint64_t` | The reading's sequence number; increases by one per reading. |
| 3 | `srtp_drops` | `int` | The legacy SRTP drop counter: every SRTP auth-tag mismatch since open(). The v1.2 transport block (`Stats::transport`) carries the same figure alongside the rest of the SRTP plane; this one stays because hosts read it. |
| 4 | `video_frames_sent` | `std::uint64_t` | send_video() frames |
| 5 | `video_packets_sent` | `std::uint64_t` | RTP packets emitted |
| 6 | `video_bytes_sent` | `std::uint64_t` | RTP bytes (hdr+payload) |
| 7 | `video_frames_received` | `std::uint64_t` | receiver frames |
| 8 | `video_nack_seqs_queued` | `std::uint64_t` | NACK seqs queued (out) |
| 9 | `video_keyframes_q` | `std::uint64_t` | keyframe reqs queued |
| 10 | `video_feedback_applied` | `std::uint64_t` | inbound RTCP actions |
| 11 | `video_keyframes_applied` | `std::uint64_t` | inbound keyframe reqs |
| 12 | `video_feedback_rx_pli` | `std::uint64_t` | PLI (picture-loss indication) the peer sent us. |
| 13 | `video_feedback_rx_fir` | `std::uint64_t` | FIR (full intra request) the peer sent us. |
| 14 | `video_feedback_rx_nack` | `std::uint64_t` | NACK (retransmission request) the peer sent us. |
| 15 | `video_feedback_rx_remb` | `std::uint64_t` | REMB (receiver bandwidth estimate) the peer sent us. |
| 16 | `video_feedback_rx_transport_cc` | `std::uint64_t` | transport-cc (RFC 8888) reports the peer sent us. |
| 17 | `video_feedback_tx_pli` | `std::uint64_t` | PLI we sent the peer. |
| 18 | `video_feedback_tx_nack` | `std::uint64_t` | NACK we sent the peer. |
| 19 | `video_keyframes_limiter_requests` | `std::uint64_t` | offered |
| 20 | `video_keyframes_granted` | `std::uint64_t` | granted |
| 21 | `video_keyframes_deferred` | `std::uint64_t` | held back |
| 22 | `video_keyframes_rejected` | `std::uint64_t` | budget spent |
| 23 | `video_keyframes_forced` | `std::uint64_t` | bypassed |
| 24 | `video_incremental_frames_seen` | `std::uint64_t` | -- |
| 25 | `video_incremental_unchanged` | `std::uint64_t` | -- |
| 26 | `video_incremental_encoded` | `std::uint64_t` | -- |
| 27 | `video_incremental_full` | `std::uint64_t` | -- |
| 28 | `video_incremental_fallbacks` | `std::uint64_t` | -- |
| 29 | `video_incremental_skipped` | `std::uint64_t` | -- |
| 30 | `video_region_refreshes_sent` | `std::uint64_t` | -- |
| 31 | `video_zero_copy_frames` | `std::uint64_t` | Frames that took the GPU path (no CPU staging). |
| 32 | `video_zero_copy_fallbacks` | `std::uint64_t` | Frames that carried a GPU handle but fell back to the CPU. |
| 33 | `video_rx_gpu_frames` | `std::uint64_t` | -- |
| 34 | `video_rx_cpu_frames` | `std::uint64_t` | -- |
| 35 | `video_zero_copy_rejected` | `std::uint64_t` | A frame that carried a GPU surface the encoder could not consume at all, and that had no CPU pixels to fall back to. The frame was dropped -- this counter makes that visible. |
| 36 | `video_capture_frames` | `std::uint64_t` | frames pulled from the device |
| 37 | `video_capture_frames_pumped` | `std::uint64_t` | frames handed to the encoder |
| 38 | `video_capture_dropped` | `std::uint64_t` | queue-overflow drops |
| 39 | `video_capture_device_errors` | `std::uint64_t` | device-lost / read errors |
| 40 | `video_capture_restarts` | `std::uint64_t` | times the device was reopened |
| 41 | `video_capture_format_changes` | `std::uint64_t` | negotiated format changed |
| 42 | `video_capture_bytes_copied` | `std::uint64_t` | CPU<->GPU bytes (0 = zero-copy) |
| 43 | `video_codec_backend_id` | `std::string` | e.g. "nvenc", "h264" |
| 44 | `video_codec_hardware` | `bool` | a hardware encoder |
| 45 | `video_codec_zero_copy` | `bool` | the last frame skipped the CPU |
| 46 | `video_codec_bitrate_bps` | `std::uint32_t` | the bitrate it was asked for |
| 47 | `video_codec_actual_bps` | `std::uint32_t` | the bitrate it produced |
| 48 | `video_codec_encode_latency_us` | `std::uint32_t` | average encode latency |
| 49 | `video_codec_frames_encoded` | `std::uint64_t` | -- |
| 50 | `video_codec_bytes_encoded` | `std::uint64_t` | -- |
| 51 | `video_codec_encode_errors` | `std::uint64_t` | -- |
| 52 | `video_codec_zero_copy_frames` | `std::uint64_t` | frames that skipped the CPU |
| 53 | `video_codec_staged_frames` | `std::uint64_t` | frames staged through the CPU |
| 54 | `video_freeze_count` | `std::uint64_t` | Number of freezes detected (a gap > `freeze_threshold_ms` between two rendered frames). |
| 55 | `video_freeze_total_ms` | `std::uint64_t` | Total frozen time in milliseconds (sum of the freeze gaps). |
| 56 | `video_last_freeze_ms` | `std::uint64_t` | Duration of the most recent freeze in milliseconds. |
| 57 | `video_keyframe_interval_avg_ms` | `double` | Running mean of the inter-keyframe distance, in milliseconds (EMA). Zero when fewer than two keyframes were seen. |
| 58 | `video_qp_sum` | `std::uint64_t` | Sum of the per-frame QP samples (0 = best quality). These are the *outbound* samples — the QP the encoder chose for the frames we sent. The average QP is `video_qp_sum / video_qp_count`. |
| 59 | `video_qp_count` | `std::uint64_t` | Number of per-frame QP samples (i.e. frames with a known QP). Zero means the codec does not report a QP — not that the quality was perfect. |
| 60 | `video_frames_dropped_jb` | `std::uint64_t` | Frames dropped at the jitter buffer (never assembled). |
| 61 | `video_frames_dropped_decoder` | `std::uint64_t` | Frames dropped by the decoder (corrupt / undecodable). |
| 62 | `video_frames_dropped_dependency` | `std::uint64_t` | Frames dropped because a reference frame was lost. This is a subset of `video_frames_dropped_decoder`. |
| 63 | `video_current_bitrate_bps` | `std::uint32_t` | The current inbound video bitrate, in bits per second (EMA over a one-second window). |
| 64 | `video_avg_bitrate_bps` | `std::uint32_t` | The session-average inbound video bitrate, in bits per second. |
| 65 | `video_bytes_received` | `std::uint64_t` | Bytes of encoded video received since open(). |
| 66 | `video_frames_decoded` | `std::uint64_t` | Frames successfully decoded by the codec. |
| 67 | `video_frames_rendered` | `std::uint64_t` | Frames handed to the video sink (rendered). |
| 68 | `video_tx_frames_encoded` | `std::uint64_t` | -- |
| 69 | `video_tx_bytes_encoded` | `std::uint64_t` | -- |
| 70 | `video_tx_frames_dropped_dependency` | `std::uint64_t` | Dependency losses the *peer* reported about our stream. A PLI / FIR the peer sends is the peer saying "a reference frame of yours never arrived, so I cannot decode what follows". This is our *transmit* side losing a reference frame — the counterpart of the inbound `video_frames_dropped_dependency`. It is counted here rather than as an inbound drop because the two describe opposite directions: the inbound counter says "we could not decode what the peer sent", this one says "the peer could not decode what we sent". |
| 71 | `audio_frames_sent` | `std::uint64_t` | RTP packets emitted |
| 72 | `audio_bytes_sent` | `std::uint64_t` | RTP bytes (hdr+payload) |
| 73 | `audio_encode_errors` | `std::uint64_t` | frames that failed to encode |
| 74 | `audio_samples_in` | `std::uint64_t` | PCM samples handed to send_audio() |
| 75 | `audio_samples_pending` | `std::uint64_t` | samples still in the accumulator |
| 76 | `audio_packets_sent` | `std::uint64_t` | RTP packets emitted |
| 77 | `audio_frames_received` | `std::uint64_t` | decoded frames delivered |
| 78 | `audio_samples_received` | `std::uint64_t` | samples per channel delivered |
| 79 | `audio_concealed_frames` | `std::uint64_t` | PLC frames delivered |
| 80 | `audio_fec_recovered_frames` | `std::uint64_t` | frames rebuilt from in-band FEC |
| 81 | `audio_packets_lost` | `std::uint64_t` | inbound packets inferred lost |
| 82 | `audio_decode_errors` | `std::uint64_t` | decoder errors |
| 83 | `audio_jitter_ms` | `std::uint32_t` | current interarrival jitter |
| 84 | `audio_playout_delay_ms` | `std::uint32_t` | current playout delay |
| 85 | `audio_encoder_bitrate_bps` | `std::uint32_t` | -- |
| 86 | `net_feedback_blocks` | `std::uint64_t` | RTCP blocks routed to the collector |
| 87 | `bwe_feedback_samples` | `std::uint64_t` | samples handed to IBwe::on_feedback() |
| 88 | `bandwidth_estimate_bps` | `std::uint32_t` | current BWE target, in bit/s |
| 89 | `dtmf_packets_sent` | `std::uint64_t` | outbound event packets |
| 90 | `dtmf_packets_received` | `std::uint64_t` | inbound event packets decoded |
| 91 | `dtmf_packets_malformed` | `std::uint64_t` | inbound packets that failed to decode |
| 92 | `dtmf_events_delivered` | `std::uint64_t` | completed events reported to the host |
| 93 | `dtmf_payload_type` | `std::uint8_t` | the negotiated telephone-event PT |
| 94 | `xr_report_blocks` | `std::uint64_t` | XR blocks routed to the consumer |
| 95 | `video_capture_device_frames_captured` | `std::uint64_t` | -- |
| 96 | `video_capture_device_frames_dropped` | `std::uint64_t` | -- |
| 97 | `video_capture_device_error_count` | `std::uint64_t` | -- |
| 98 | `video_capture_device_restarts` | `std::uint64_t` | -- |
| 99 | `video_capture_device_format_changes` | `std::uint64_t` | -- |
| 100 | `video_capture_device_bytes_copied` | `std::uint64_t` | -- |
| 101 | `video_capture_device_frames_zero_copy` | `std::uint64_t` | -- |
| 102 | `video_capture_device_frames_cpu_staged` | `std::uint64_t` | -- |
| 103 | `video_capture_pumped_frames` | `std::uint64_t` | -- |
| 104 | `video_capture_pump_errors` | `std::uint64_t` | -- |
| 105 | `video_capture_pumped_metadata` | `std::uint64_t` | -- |
| 106 | `video_capture_gpu_frames` | `std::uint64_t` | -- |
| 107 | `video_capture_gpu_errors` | `std::uint64_t` | -- |
| 108 | `video_capture_gpu_wired` | `bool` | -- |
| 109 | `video_format_conversions` | `std::uint64_t` | -- |
| 110 | `video_format_copies` | `std::uint64_t` | The frames that were copied without a colour conversion. |
| 111 | `video_format_zero_copy` | `std::uint64_t` | The frames that reached the encoder as a native GPU surface (so no format work happened at all). |
| 112 | `video_format_canonical` | `std::string` | The canonical format the encoder is fed (e.g. "i420"). |
| 113 | `video_format_source` | `std::string` | The format the capture device most recently delivered (e.g. "bgra"). A host compares it with the canonical format to see whether a conversion is happening. |
| 114 | `video_codec_backend` | `std::string` | The codec backend's id ("nvenc", "openh264", "stub_h264", ...). "unknown" when the codec does not name its backend. |
| 115 | `video_codec_hardware_accelerated` | `bool` | True iff the codec runs on a hardware accelerator. |
| 116 | `video_codec_backend_preference` | `std::string` | The backend class the host asked for (plan B2). "auto" / "hardware" / "software" — the value of `EngineConfig::video_codec_backend`. Read this together with `video_codec_backend`: a host that asked for "hardware" and got "openh264_h264" has a problem the engine would otherwise hide, and this field makes the mismatch visible without parsing a log. |
| 117 | `video_codec_frames_zero_copy` | `std::uint64_t` | Frames the codec encoded through the GPU zero-copy path. |
| 118 | `video_codec_frames_staged` | `std::uint64_t` | Frames the codec encoded through the CPU staging path. |
| 119 | `video_codec_keyframes` | `std::uint64_t` | Keyframes (IDR) the codec produced. |
| 120 | `video_codec_bytes_copied` | `std::uint64_t` | Bytes the codec copied CPU↔GPU (0 on a pure hardware path). |
| 121 | `video_codec_encode_latency_avg_us` | `std::uint64_t` | The mean encode latency, in microseconds. |
| 122 | `video_codec_encode_latency_max_us` | `std::uint64_t` | The largest single encode duration, in microseconds. |
| 123 | `video_tx_bitrate_bps` | `std::uint32_t` | The current outbound video bitrate, in bits per second. Derived from the encoded byte count over the last second. |
| 124 | `video_sink_frames_rendered` | `std::uint64_t` | -- |
| 125 | `video_sink_frames_dropped` | `std::uint64_t` | -- |
| 126 | `video_sink_frames_dropped_present` | `std::uint64_t` | -- |
| 127 | `video_sink_frames_presented` | `std::uint64_t` | -- |
| 128 | `video_sink_render_errors` | `std::uint64_t` | -- |
| 129 | `video_sink_present_fps` | `std::uint32_t` | -- |
| 130 | `video_sync` | `VideoSyncStats` | -- |
| 131 | `video_streams` | `std::vector<VideoStreamStats>` | -- |
| 132 | `inbound_rtp_streams` | `std::vector<InboundRtpStats>` | -- |
| 133 | `audio_streams` | `std::vector<AudioStreamStats>` | -- |
| 134 | `transport` | `TransportStats` | The ICE / DTLS / SRTP path the session runs over. |
| 135 | `data_channels` | `std::vector<DataChannelStats>` | ---- The per-channel data-channel entries (v1.2 plan Track S / S6) One entry per **data channel the host opened** with `create_data_channel()`, each with its own identity (the engine-assigned id and the label) and its own counters. The list is empty when the host never opened a channel, so a host that does not use the data plane sees no change in its telemetry. It is a list rather than a single struct so that the *shape* survives the arrival of a second channel — the alternative is to redraw the whole surface then, and every caller with it. |
| 136 | `video_stage_capture` | `StageTiming` | The time spent popping the frame from the capture device. |
| 137 | `video_stage_convert` | `StageTiming` | The time spent converting the picture (colour / format / scale). |
| 138 | `video_stage_encode` | `StageTiming` | The time spent inside the codec's encode call. |
| 139 | `video_stage_packetize` | `StageTiming` | The time spent packetizing (RTP header, STAP-A / FU-A, SRTP). |
| 140 | `video_stage_decode` | `StageTiming` | The time spent inside the codec's decode call. |
| 141 | `video_stage_render` | `StageTiming` | The time spent in the sink's render call. |
| 142 | `video_stage_total_us` | `std::uint64_t` | The summed cost of the outbound path, in microseconds. This is the number §B.6's budget is written against: the capture → convert → encode → packetize chain. It is the sum of the four stage means above, so a host can compare it with the budget directly: ``` const auto s = engine.stats(); const auto frame_cost_us = s.video_stage_capture.mean_us + s.video_stage_convert.mean_us + s.video_stage_encode.mean_us + s.video_stage_packetize.mean_us; ``` The p95 variant (`video_stage_total_p95_us`) sums the p95s instead — an upper bound on the *typical* worst frame, which is the number to compare with the frame interval (33 333 µs at 30 fps). |
| 143 | `video_stage_total_p95_us` | `std::uint64_t` | The summed p95 of the four outbound stages, in microseconds. |
| 144 | `qoe` | `plugins::QoeSnapshot` | The quality-of-experience metrics (plan J3 / J6). The freeze count, the end-to-end latency percentiles, the viewer's frame rate, the sharpness proxy and the input-to-photon latency. The metrics are measured whenever a QoE collector is resolved — which, by default, is the built-in `"rd-qoe"` (see `EngineConfig::qoe_collector_name`); a host that turns the metrics off says so with `EngineConfig::qoe_collector_enabled` (or with an empty name). Without a collector this snapshot carries the **frame counters** — the presented and dropped frame counts, which the engine keeps anyway — and zeroes for the metrics, which is the honest answer to "what was the latency percentile?" when it was not measured. |
| 145 | `remote_desktop` | `RemoteDesktopStats` | The remote-desktop pipeline breakdown (v1.2 plan **S7**). The QoE block above answers "how did the session *feel*"; this block answers "**where did the time go**". It is a remote-desktop-shaped view of the per-stage instrumentation (`video_stage_*`): the capture / encode / decode means in milliseconds, the frame rates they imply, and the count of frames that were captured but never encoded (the gap between two stages is a frame that was dropped in between). Every value here is **derived** from a measurement that already exists — nothing is timed a second time. The definitions are in `RemoteDesktopStats` above. |

**145 fields.**

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

## 16. Migration guide — the audio pipeline (v1.2.0)

The audio pipeline's public SDK is **additive**.  An existing host
keeps compiling and keeps working; this section says what it *gains*
and what it *should change*.

### 16.1 Nothing must change

A host that only uses `set_on_audio_frame()` and `send_audio()`
continues to work unchanged.

**One behavioural change is worth calling out.**  The legacy callback
now receives genuinely **decoded** PCM.  Before v1.2.0 the receive
path handed the host the compressed RTP payload reinterpreted as
floating-point samples, so a host that appeared to "work" against the
old build was in fact processing garbage.  After the change it
processes real audio.  This is a **fix, not a regression** — but the
bytes on the wire changed, and a host that had grown a workaround
around the noise may need to drop it.

### 16.2 Recommended migration

Install the new callback, and keep the legacy one only if a legacy
consumer still needs it:

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

### 16.3 Buffer lifetime

`f.pcm` points into a decoder-owned buffer that is reused for the next
frame.  A host that needs to keep the audio must copy it.  This
matches the existing contract of the legacy callback and is stated
explicitly because it is the most common integration mistake.

### 16.4 Tick rate

The host must call `tick()` at **≥ 100 Hz** for audio playout to be
smooth.  A host that calls it at 30 Hz still gets continuous audio,
with coarser granularity — the playout follows the media time, not the
arrival, so a slower tick delays the output instead of dropping it.

### 16.5 Reading the negotiated format

See §8.3 — `remote_audio_format()`.

### 16.6 Backwards-compatibility rules

- The legacy callback fires **before** the new one, with the same
  buffer.
- Installing the new callback does **not** uninstall the legacy one,
  and vice versa.  Both may be absent.
- When neither is installed, the receiver still runs — the decoding
  happens regardless, because the statistics and the 3A render
  reference depend on it.  The frames are simply dropped.

---

## 17. See also

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
