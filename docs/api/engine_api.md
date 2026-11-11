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
* the tightest limit wins — a looser TMMBR never raises a ceiling a
  tighter one has set (RFC 5104 section 3.5.4.2);
* a tuple naming a media source the engine does not send is recorded
  but **not** applied: it is somebody else's constraint.

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

The remaining fields are **cumulative counters since `open()`**, except
`audio_encoder_bitrate_bps` and `bandwidth_estimate_bps`, which are the
**instantaneous** values (the outbound audio encoder's current bitrate and
the bandwidth estimator's current target respectively). `audio_jitter_ms`
and `audio_playout_delay_ms` are likewise instantaneous measurements
rather than running totals.

**This is not a standard `RTCStatsReport`.** The struct is flat and
engine-wide: there is no per-stream breakdown, no snapshot timestamp, and
no ICE candidate-pair / RTT dimension. The `src/modules/video_stats/`
module provides freeze counts, keyframe intervals, QP and drop-cause
breakdown, but it is **not wired into the engine** yet, so those fields
are not reachable through this accessor. For the full picture — the
planned stream-level shape and the missing dimensions — see
`docs/plan/stats-surface-plan.md` (landed later in the v1.2 stack,
so referenced here by path, not by link).

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
