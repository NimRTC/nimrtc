# ADR-017: Cross-end frame timeline — RFC 6051 abs-send-time + RTCP-SR anchoring

| | |
|---|---|
| Number | 017 |
| Status | **Accepted (seam landed, feature incomplete)** |
| Date | 2026-10-21 |
| Phase | P2 (v1.0.0) |
| Bound to | v1.0.0 |
| Supersedes | — |
| Related | `docs/plan/v1.0-plan.md` §8 (Timeline expectation), `docs/adr/ADR-015-api-stability-policy.md` (the `NIMRTC_API_STABLE` markers on the seam), `docs/adr/ADR-001-plugin-system.md` (the plugin surface it feeds), `src/modules/rtp/include/nimrtc/rtp/timeline.hpp` (the implementation) |

> **ADR-016 is deliberately not this ADR.** Number 016 is reserved by
> `docs/plan/v1.0-plan.md` §12.5 for the (currently deferred, SFU-track)
> "SFU forwarding skips L2" BWE-interface ADR. The timeline seam took
> 017 so that reservation stays intact.

## Context

`Profile::timeline_enabled` has existed since v0.x and is set `true` in
seven shipped profiles (`agent`, `teleop`, `agent-gateway`,
`agent-low-latency`, `relay-agent`, `sfu-agent`, plus the
`kProfileClosedLoop` preset in `assembly/profiles.hpp`). The flag is
parsed, serialised, and round-trip-tested — but until this seam there
was **nothing behind it**. Consumers asking "when was this frame
captured, in a clock both ends agree on?" had no engine API.

Three mechanisms exist in RFC-land to answer it, and the codebase had
partial support for two of them without a way to join them:

1. **RTCP Sender Report** (RFC 3550 §6.4.1) pairs an NTP wall-clock
   with an RTP media clock. `Parser::parse_sr()` already decoded it
   into a `SenderReport` struct — with nothing downstream consuming it.
2. **abs-send-time** (RFC 6051 / Chrome's
   `http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time`) — a
   24-bit timestamp at 65536 Hz carried in every RTP header. The RTP
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
`Profile::timeline_enabled`, carrying RFC 6051 abs-send-time on the
wire and RFC 3550 SR-based NTP↔RTP-TS mapping for reconstruction.**

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
exists) but the v1.0.0 implementation resolves capture time from the
most-recent SR plus a linear RTP-TS delta. **Combining the two — using
abs-send-time to reject or smooth SR-derived timestamps — is
explicitly out of scope for v1.0.0** and is the first item of the
completion work below.

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
SR ingestion and single-SR linear extrapolation; 9 unit cases in
`src/modules/rtp/tests/test_timeline.cpp`.

**Not landed — the feature is a seam, not a capability.** Nothing
consumes `SenderReportTracker` outside its own tests. Specifically
still to do before `timeline_enabled=true` means anything end-to-end:

| Gap | Where it belongs |
|---|---|
| Per-SSRC clock rate from SDP `rtpmap` (currently hardcoded 90000 — wrong for audio at 48000 Hz) | `SenderReportTracker::on_sender_report` |
| RTP-timestamp wrap handling (the `> 2^31` branch currently refuses the legitimate post-wrap forward case instead of adding 2³²) | `capture_ts_us_for` |
| `abs_send_time_24` actually feeding the estimate (Decision 2) | `capture_ts_us_for` |
| `monotonic_origin_ntp64` actually anchoring encode/decode (currently `(void)`-discarded in both) | `timeline.cpp` |
| extmap negotiation + header-extension write, using `kAbsSendTimeUri` / `kAbsSendTimeDefaultLocalId` (both declared, zero references) | engine / RTP builder |
| SR send scheduling on `kSrPeriodMs` (declared, zero references) | engine |
| Engine wiring: read `Profile::timeline_enabled`, feed parsed SRs into the tracker, expose capture timestamps on the plugin surface | `src/engine` |
| `sender_recv_skew_us` real value | v1.1 |

## Consequences

**Positive.** The public seam exists and is frozen under ADR-015
(`NIMRTC_API_STABLE`), so v1.1+ completion does not break v1.0.0
consumers. Seven profiles can set `timeline_enabled` without a
signature change. The anchoring convention and the SR-authoritative /
abs-send-time-refining split are decided, which is the part that is
expensive to reverse.

**Negative.** `timeline_enabled=true` is currently **inert**. A
consumer enabling it and expecting capture timestamps will get
nothing; the flag still only costs the parse/serialise round trip. The
three declared-but-unreferenced constants (`kSrPeriodMs`,
`kAbsSendTimeUri`, `kAbsSendTimeDefaultLocalId`) are a standing
reminder, not working configuration. The hardcoded 90 kHz clock rate
means the tracker is correct for video only.

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
- **Reuse RFC 9143 abs-send-time as the sole clock.** Rejected: the
  256 s wrap makes it unusable alone for anything but short sessions.
- **Adopt an existing NTP↔RTP mapper.** No in-tree option; the ~40
  lines of linear extrapolation are smaller than the dependency.