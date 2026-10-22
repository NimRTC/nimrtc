# ADR-017: Cross-end frame timeline — abs-send-time + RTCP-SR anchoring

| | |
|---|---|
| Number | 017 |
| Status | **Accepted (landed; audio clock rate pending, see Decision 6)** |
| Date | 2026-10-21 |
| Phase | P2 (v1.0.0) |
| Bound to | v1.0.0 |
| Supersedes | — |
| Related | `docs/plan/v1.0-plan.md` §8 (Timeline expectation), `docs/plan/timeline-engine-wiring.md` (engine-side landing plan + open gaps), `docs/adr/ADR-015-api-stability-policy.md` (the `NIMRTC_API_STABLE` markers on the seam), `docs/adr/ADR-001-plugin-system.md` (the plugin surface it feeds), `src/modules/rtp/include/nimrtc/rtp/timeline.hpp` (the implementation) |

> **ADR-016 is deliberately not this ADR.** Number 016 is reserved by
> `docs/plan/v1.0-plan.md` §12.5 for the (currently deferred, SFU-track)
> "SFU forwarding skips L2" BWE-interface ADR. The timeline seam took
> 017 so that reservation stays intact.

## Context

`Profile::timeline_enabled` has existed since v0.x and is set `true` in
six shipped JSON profiles (`agent`, `teleop`, `agent-gateway`,
`agent-low-latency`, `relay-agent`, `sfu-agent`) and in five C++
presets in `assembly/profiles.hpp` (`kProfileAgent`, `kProfileTeleop`,
`kProfileAgentGateway`, `kProfileRelayAgent`,
`kProfileAgentLowLatency`; the JSON set additionally enables
`sfu-agent`). The flag is parsed, serialised, and round-trip-tested —
but until this seam there was **nothing behind it**. Consumers asking
"when was this frame captured, in a clock both ends agree on?" had no
engine API.

Three mechanisms exist in RFC-land to answer it, and the codebase had
partial support for two of them without a way to join them:

1. **RTCP Sender Report** (RFC 3550 §6.4.1) pairs an NTP wall-clock
   with an RTP media clock. `Parser::parse_sr()` already decoded it
   into a `SenderReport` struct — with nothing downstream consuming it.
2. **abs-send-time** (the WebRTC header extension
   `http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time`, from the
   never-published `draft-ietf-avtcore-abs-send-time`) — a 24-bit
   timestamp at 65536 Hz carried in every RTP header. The RTP
   parser/builder already round-tripped the extension element
   (`parse_abs_send_time_extension` / `build_abs_send_time_extension`,
   `test_transport_cc.cpp`), but nothing tied it to a wall clock.
3. **RFC 3550 §8.1 NTP↔RTP-TS mapping** — the actual arithmetic that
   turns (1) into a per-packet capture time. Absent entirely.

These must agree. Chrome's NetEq and libwebrtc both derive capture
timestamps from the pair, and a receiver that mixes the two — or
ignores the RTP header extension and relies on arrival time — cannot
do closed-loop latency control for the agent pipelines the profiles
above were written for.

## Decision

**NimRTC introduces a cross-end timeline seam at
`src/modules/rtp/include/nimrtc/rtp/timeline.hpp`, gated by
`Profile::timeline_enabled`, carrying WebRTC abs-send-time on the wire
and RFC 3550 SR-based NTP↔RTP-TS mapping for reconstruction.**

### Decision 1 — Anchoring convention: Unix epoch, not NTP epoch

`capture_ts_us_for()` returns **microseconds since 1970-01-01 UTC**,
not since the 1900 NTP epoch. The SR's `ntp_timestamp` seconds field
is reduced by `kNtpEpochOffset` (`2208988800`); a pre-1970 SR is
clamped to 0 seconds with its fraction preserved.

This matches Chrome's NetEq / `RtpDepacketizer`, and it means a
caller can subtract its own monotonic clock (cast to Unix-microseconds
via boot time) directly, with no second conversion. Storing the
*delta* (sender wall clock − local monotonic at receive) rather than
the absolute Unix number remains the recommended pattern for
end-to-end capture-vs-display alignment, because the absolute number
inherits the two ends' clock-offset error.

> This was corrected after first review: the initial implementation
> documented and returned NTP-anchored microseconds, which forced
> every caller to remember the 70-year offset.

### Decision 2 — abs-send-time is carried, SR is authoritative

The 24-bit abs-send-time field wraps every 256 s and is therefore not
self-sufficient for wall-clock reconstruction. It is
**quantisation-only**: it contributes ~15.26 µs of resolution, not
the epoch. The SR supplies the epoch; abs-send-time refines it.

Consequently `capture_ts_us_for()` takes the abs-send-time value in
its signature (so the seam is wire-compatible and the argument slot
exists). The v1.0.0 implementation resolves capture time from the
most-recent SR plus a linear RTP-TS delta; the **7-argument overload**
additionally combines the two: it refines the SR-derived estimate with
the abs-send-time *delta* (the difference between the packet's
abs-send-time value and the value that was current at the SR instant),
which measures the sender's elapsed time without inheriting the media
clock's rate error. The refinement is applied only when the two
estimates agree within `kAstAgreementToleranceUs` — otherwise the
SR-derived value wins, because a large disagreement means the
abs-send-time anchor is broken (e.g. a peer that restarted its 256 s
counter).

> This supersedes the original "combining the two is out of scope for
> v1.0.0" wording: the combination landed after review, in the shape
> above.

### Decision 3 — Deliberately linear, no EWMA/Kalman

The estimator is a single-SR linear extrapolation:

```
capture_us = sr_ntp_us + (rtp_ts − sr_rtp_ts) × 1e6 / clock_rate_hz
```

This is correct to sub-millisecond for sessions of a few minutes and
degrades gracefully after that. A slope *estimator* (fitting ≥ 2 SRs)
and a smoother (EWMA / Kalman) are v1.1+ work. Making this trade-off
explicit means callers who need long-session accuracy can measure the
error against `local_recv_us_since_epoch` themselves.

### Decision 4 — Skew reporting is a v1.1 API

`capture_ts_us_for()`'s `sender_recv_skew_us` out-parameter is a
`std::int64_t*` (pass `nullptr` to discard) and is **always 0** in
v1.0.0. The parameter is reserved so the API shape survives into v1.x
without a break; the value requires paired local-monotonic samples at
both the SR instant and the RTP instant, which the engine does not yet
thread through. Computing it out-of-band is the v1.0.0 contract.

> Signature note: this was a `std::int64_t&` in the first cut, which
> forced every caller — including `std::ignore` — to supply storage.
> The pointer form is the freeze-friendly shape.

### Decision 5 — Scope of the v1.0.0 landing

**Landed (this ADR):** `Ntp64` + `system_clock` conversion;
abs-send-time 24-bit encode/decode; `SenderReportTracker` with per-SSRC
SR ingestion and single-SR linear extrapolation; 21 unit cases in
`src/modules/rtp/tests/test_timeline.cpp`.

**The engine side landed with it.** `EngineConfig::timeline_enabled`
and `EngineConfig::sr_period_ms` gate the seam; the engine advertises
the extension in its SDP, parses the peer's `a=extmap`, feeds inbound
RTCP Sender Reports and inbound abs-send-time elements into the tracker,
and exposes the reconstruction through `last_capture_ts_us()`,
`capture_ts_us_for_ssrc()` and `last_one_way_delay_us()`.

| Gap | State |
|---|---|
| Per-SSRC clock rate from SDP `rtpmap` (was hardcoded 90000 — wrong for audio at 48000 Hz) | **landed** — `set_ssrc_clock_rate()` |
| RTP-timestamp wrap handling (the `> 2^31` branch refused the legitimate post-wrap forward case) | **landed** — modular difference over 2³² |
| `abs_send_time_24` actually feeding the estimate (Decision 2) | **landed** — the 7-argument `capture_ts_us_for()` overload refines the SR-based estimate from the abs-send-time delta, subject to `kAstAgreementToleranceUs` |
| `monotonic_origin_ntp64` actually anchoring encode/decode (was `(void)`-discarded in both) | **landed** — the encoder anchors to the Unix epoch; the decoder deliberately returns a position *within one 256 s cycle* (choosing the cycle is the caller's job) |
| extmap negotiation + header-extension write, using `kAbsSendTimeUri` / `kAbsSendTimeDefaultLocalId` | **landed** — `create_offer()` / `process_remote_sdp()` negotiate the per-direction id; `PacketBuilder::set_abs_send_time(t, id)` emits under it |
| SR send scheduling on `kSrPeriodMs` (declared, zero references) | **landed** — `EngineConfig::sr_period_ms` drives the send cadence |
| Engine wiring: read `Profile::timeline_enabled`, feed parsed SRs into the tracker, expose capture timestamps | **landed** — `src/engine` |
| `sender_recv_skew_us` real value | **v1.1** by decision |

### Decision 6 — Known limitation: no engine-side SDP-driven clock rate

`SenderReportTracker` needs each SSRC's media clock rate, which the
media clock rate does not carry (RFC 3550 §6.4.1) — it comes from the
SDP `a=rtpmap` line. The engine currently does **not** call
`set_ssrc_clock_rate()` per SSRC, so an audio SSRC at 48 kHz is
reconstructed against the 90 kHz default and its capture instants are
wrong by a factor of 90000/48000 ≈ 1.875.

This is a real gap, not a documented limitation to accept: until it
closes, `last_capture_ts_us()` is only trustworthy for video (90 kHz)
SSRCs. It is tracked as v1.1 work alongside `sender_recv_skew_us`, and
the fix sequence is in `docs/plan/timeline-engine-wiring.md`.

## Consequences

**Positive.** The public seam exists and is frozen under ADR-015
(`NIMRTC_API_STABLE`), so v1.1+ completion does not break v1.0.0
consumers. Six JSON profiles can set `timeline_enabled` without a
signature change. The anchoring convention and the SR-authoritative /
abs-send-time-refining split are decided, which is the part that is
expensive to reverse.

**Negative.** Reconstruction is only correct for 90 kHz (video) SSRCs
until the engine wires `set_ssrc_clock_rate()` from the SDP `a=rtpmap`
(see Decision 6). `sender_recv_skew_us` is reserved but always 0
(Decision 4). Nothing in the current test suite exercises the engine
path end-to-end — the timeline unit tests cover the RTP-side tracker
directly, so a regression in the SDP-negotiation or RTCP-demux wiring
would not be caught by `ctest`.

**Neutral.** The `set_ref_frame()` no-op kept on the video-pipeline
plugin interface is unaffected — the engine reads capture metadata
from the frame and the wire, not from that RPC.

## Alternatives considered

- **Keep the timeline logic in the engine.** Rejected: the arithmetic
  is pure, stateless-per-SSRC, and belongs beside the RTP parser that
  produces its input. Engine-local placement would also make it
  untestable without an engine instance.
- **Use arrival time as the capture estimate.** Rejected: it folds
  network jitter into the capture timestamp, which defeats the
  closed-loop latency control the agent profiles are for.
- **Reuse abs-send-time as the sole clock.** Rejected: the
  256 s wrap makes it unusable alone for anything but short sessions.
- **Adopt an existing NTP↔RTP mapper.** No in-tree option; the ~40
  lines of linear extrapolation are smaller than the dependency.